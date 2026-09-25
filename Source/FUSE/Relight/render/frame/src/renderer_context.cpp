// FUSE Relight RL-4.1: the FUSE renderer on the host's device (see renderer_context.hpp).
#include <fuse/relight/render/frame/renderer_context.hpp>

#include <fuse/relight/render/frame/bindless_renderer_heap.hpp>
#include <fuse/relight/render/frame/scene_adapter.hpp>
#include <fuse/relight/render/frame/vk_dispatch.hpp> // <vulkan/vulkan.h> (volk shim) without the DrawState macro

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/loader.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <atomic>
#include <mutex>

namespace fuse::relight::render::frame {

namespace rr = fuse::renderer;
namespace gs = fuse::renderer::gpu_scene;

namespace {

/// Captured once, before this process's first attach initialises volk (see LoaderReport).
std::atomic<int> g_loadedBeforeAttach{-1};

constexpr fuse::usize kStagingBytes = 4u * 1024u * 1024u; ///< GPU scene deltas of a frame (tiny scenes: KiB)

VkImageViewType viewTypeOf(std::uint32_t t) {
    switch (t) {
    case 2u:
        return VK_IMAGE_VIEW_TYPE_3D;
    case 3u:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    default:
        return VK_IMAGE_VIEW_TYPE_2D;
    }
}

/// Sampled views of external / FUSE images on the adopted device (volk entry points).
class DeviceViewFactory final : public IImageViewFactory {
public:
    void setDevice(VkDevice device) { m_device = device; }
    std::uint64_t createView(const ExternalImageDesc& image) override {
        if (m_device == VK_NULL_HANDLE || image.vkImage == 0 || image.vkFormat == 0) {
            return 0;
        }
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = vkHandle<VkImage>(image.vkImage);
        ci.viewType = viewTypeOf(image.viewType);
        if (ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE && image.arrayLayers != 6u) {
            ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        }
        ci.format = static_cast<VkFormat>(image.vkFormat);
        ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mipLevels ? image.mipLevels : 1u, 0,
                               ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE ? 6u : 1u};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(m_device, &ci, nullptr, &view) != VK_SUCCESS) {
            return 0;
        }
        return vkValue(view);
    }
    void destroyView(std::uint64_t view) override {
        if (m_device != VK_NULL_HANDLE && view != 0) {
            vkDestroyImageView(m_device, vkHandle<VkImageView>(view), nullptr);
        }
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
};

} // namespace

/// The adapter behind the host's queue lock: endFrame commits the GPU scene and submits its uploads.
class ContextSink final : public IGpuSceneSink {
public:
    explicit ContextSink(RendererContext& context) : m_context(context) {}
    void beginFrame(std::uint64_t serial) override { m_context.adapter().beginFrame(serial); }
    void submit(const AdapterDraw& draw) override { m_context.adapter().submit(draw); }
    void submitLights(const std::vector<AdapterLight>& lights) override { m_context.adapter().submitLights(lights); }
    void endFrame() override {
        // commit() stages and records copies into the upload queue (which may submit when its ring is full):
        // both under the queue lock, like the flush.
        m_context.lockQueue();
        m_context.adapter().endFrame();
        m_context.unlockQueue();
        m_context.flushUploads();
    }
    void clear() override {
        m_context.lockQueue();
        m_context.adapter().clear();
        m_context.unlockQueue();
        m_context.flushUploads();
    }
    std::uint32_t instanceCount() const override { return m_context.adapter().instanceCount(); }

private:
    RendererContext& m_context;
};

struct RendererContext::Impl {
    tap::IFrameHost* host = nullptr;
    std::unique_ptr<rr::VulkanDevice> device;
    std::unique_ptr<rr::GpuAllocator> allocator;
    rr::BindlessDescriptors bindless;
    bool bindlessInit = false;
    rr::Buffer staging{};
    rr::UploadQueue upload;
    bool uploadInit = false;
    gs::GpuScene scene;
    bool sceneInit = false;
    std::unique_ptr<GpuSceneAdapter> adapter;
    std::unique_ptr<RendererBindlessHeap> heap;
    DeviceViewFactory views;
    std::unique_ptr<ContextSink> sink;
    RendererContextStats stats;
};

RendererContext::RendererContext() : m_impl(std::make_unique<Impl>()) {}
RendererContext::~RendererContext() { detach(); }

bool RendererContext::attached() const { return m_impl->stats.adopted; }

LoaderReport RendererContext::loaderReport() {
    LoaderReport r;
    r.autoInitLinked = rr::vkloader::autoInitialized();
    r.loadedBeforeAttach = g_loadedBeforeAttach.load() == 1;
    r.throughProcAddr = rr::vkloader::loadedThroughProcAddr();
    r.loaded = rr::vkloader::loaderLoaded();
    return r;
}

bool RendererContext::attach(tap::IFrameHost& host, GpuSceneAdapterTextureFn textureHandle) {
    detach();
    Impl& d = *m_impl;
    d.stats = RendererContextStats{};
    auto fail = [&](std::string why) {
        d.stats.error = std::move(why);
        detach();
        d.stats.adopted = false;
        return false;
    };
    tap::HostDeviceInfo info;
    if (!host.deviceCreateInfo(info)) {
        d.stats.error = "the host does not describe its device (not FUSE's bootstrap device)";
        return false;
    }
    int expected = -1;
    g_loadedBeforeAttach.compare_exchange_strong(expected, rr::vkloader::loaderLoaded() ? 1 : 0);
    // The first loader wins: in d3d9.dll (no auto-init) this is the host's own entry point.
    void* gipa = reinterpret_cast<void*>(static_cast<std::uintptr_t>(info.getInstanceProcAddr));
    if (!rr::vkloader::initializeWithProcAddr(gipa)) {
        return fail("volk: initializeWithProcAddr failed");
    }
    d.host = &host;

    rr::VulkanDeviceAdoptDesc a;
    a.instance = reinterpret_cast<void*>(static_cast<std::uintptr_t>(info.instance));
    a.physicalDevice = reinterpret_cast<void*>(static_cast<std::uintptr_t>(info.physicalDevice));
    a.device = reinterpret_cast<void*>(static_cast<std::uintptr_t>(info.device));
    a.instanceApiVersion = info.instanceApiVersion;
    a.enabledExtensions = info.enabledExtensions;
    a.enabledExtensionCount = info.enabledExtensionCount;
    a.instanceExtensions = info.instanceExtensions;
    a.instanceExtensionCount = info.instanceExtensionCount;
    a.enabledFeatureChain = info.enabledFeatureChain;
    a.enabledCoreFeatures = info.enabledCoreFeatures;
    a.graphicsFamily = info.queueFamily;
    a.graphicsQueue = reinterpret_cast<void*>(static_cast<std::uintptr_t>(info.queue));
    a.takeOwnership = false;
    a.getInstanceProcAddr = gipa;
    d.device = rr::VulkanDevice::adopt(a);
    if (!d.device || !d.device->isValid()) {
        return fail("VulkanDevice::adopt failed: " + (d.device ? d.device->info().message : std::string("null")));
    }
    const rr::VulkanDeviceInfo& di = d.device->info();
    d.stats.device = di.deviceName;
    d.stats.tier = static_cast<std::uint32_t>(di.caps.tier);
    d.stats.hardwareTier = static_cast<std::uint32_t>(di.caps.hardwareTier);
    if (!di.bufferDeviceAddress || !di.timelineSemaphore) {
        return fail("the adopted device lacks buffer device address / timeline semaphores");
    }
    d.views.setDevice(static_cast<VkDevice>(d.device->nativeHandle()));

    d.allocator = rr::GpuAllocator::create(*d.device);
    if (!d.allocator || !d.allocator->isValid()) {
        return fail("GpuAllocator on the adopted device failed");
    }
    d.bindless.init(*d.device, rr::BindlessDesc{});
    d.bindlessInit = true;
    d.stats.bindlessBackend = rr::bindlessBackendName(d.bindless.backend());
    d.stats.gpuDescriptors = d.bindless.vulkanDescriptorsReady();
    if (!d.stats.gpuDescriptors) {
        return fail("the bindless heap has no GPU descriptors on the adopted device");
    }

    rr::BufferDesc staging;
    staging.size = kStagingBytes;
    staging.usage = rr::BufferUsage::TransferSrc;
    staging.memoryUsage = rr::MemoryUsage::CpuToGpu;
    staging.name = "relight.frame.staging";
    if (!d.allocator->createBuffer(staging, d.staging) || d.staging.mapped == nullptr) {
        return fail("staging ring allocation failed");
    }
    if (!d.upload.init(d.device.get(), d.staging.handle, d.staging.mapped, kStagingBytes)) {
        return fail("UploadQueue on the adopted device failed");
    }
    d.uploadInit = true;

    gs::GpuSceneDesc sd;
    sd.device = d.device.get();
    sd.allocator = d.allocator.get();
    sd.upload = &d.upload;
    sd.bindless = &d.bindless;
    sd.scatter = gs::GpuSceneScatter::Off; // direct copies: nothing but the upload batch touches the tables
    sd.name = "relight.gpu_scene";
    if (!d.scene.init(sd)) {
        return fail("GpuScene GPU init failed");
    }
    d.sceneInit = true;
    d.stats.gpuScene = d.scene.gpuEnabled();
    d.adapter = std::make_unique<GpuSceneAdapter>(d.scene, std::move(textureHandle));
    d.heap = std::make_unique<RendererBindlessHeap>(d.bindless);
    d.sink = std::make_unique<ContextSink>(*this);
    d.stats.adopted = true;
    return true;
}

void RendererContext::detach() {
    Impl& d = *m_impl;
    if (d.uploadInit) {
        d.upload.destroy(); // waits for its in-flight batches
        d.uploadInit = false;
    }
    d.sink.reset();
    d.adapter.reset();
    if (d.sceneInit) {
        d.scene.destroy();
        d.sceneInit = false;
    }
    if (d.allocator && d.staging.handle) {
        d.allocator->destroyBuffer(d.staging);
    }
    d.staging = rr::Buffer{};
    d.heap.reset();
    if (d.bindlessInit && d.device) {
        d.bindless.destroy(*d.device);
        d.bindlessInit = false;
    }
    d.allocator.reset();
    d.views.setDevice(VK_NULL_HANDLE);
    d.device.reset(); // non-owning: unregisters the device from volk, never destroys it
    d.host = nullptr;
    d.stats.adopted = false;
}

rr::VulkanDevice& RendererContext::device() { return *m_impl->device; }
rr::GpuAllocator& RendererContext::allocator() { return *m_impl->allocator; }
rr::BindlessDescriptors& RendererContext::bindless() { return m_impl->bindless; }
gs::GpuScene& RendererContext::scene() { return m_impl->scene; }
GpuSceneAdapter& RendererContext::adapter() { return *m_impl->adapter; }
IBindlessHeap& RendererContext::heap() { return *m_impl->heap; }
IImageViewFactory& RendererContext::views() { return m_impl->views; }
IGpuSceneSink& RendererContext::sink() { return *m_impl->sink; }
const RendererContextStats& RendererContext::stats() const { return m_impl->stats; }

void RendererContext::setRetireSerial(std::uint64_t serial) {
    if (attached()) {
        m_impl->bindless.setFrameSerial(serial);
    }
}

void RendererContext::lockQueue() {
    if (m_impl->host) {
        m_impl->host->lockQueue();
        ++m_impl->stats.queueLocks;
    }
}

void RendererContext::unlockQueue() {
    if (m_impl->host) {
        m_impl->host->unlockQueue();
    }
}

bool RendererContext::flushUploads() {
    Impl& d = *m_impl;
    if (!d.uploadInit) {
        return true;
    }
    const std::uint64_t before = d.upload.stats().submittedBatches;
    lockQueue();
    const rr::UploadTicket t = d.upload.flush();
    unlockQueue();
    d.stats.uploadBatches += d.upload.stats().submittedBatches - before;
    return t.isValid();
}

void RendererContext::collect(std::uint64_t completed) {
    Impl& d = *m_impl;
    if (!attached()) {
        return;
    }
    d.scene.collectRetired(completed);
    d.bindless.collectRetired(completed);
}

} // namespace fuse::relight::render::frame
