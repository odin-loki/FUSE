// GPU skinning in the GPU scene ("skin.lbs", skinning/skinning_gpu.hpp). Vulkan gates (Lavapipe and the RTX 3090; every
// validation / synchronization-validation message fails the run in the validated modes):
//
//   --mode parity      GpuScene with 3 meshlet meshes (UV sphere, torus, box), 7 skinned instances over 2 skin meshes
//                      (4-bone and 6-bone chains along y, 2- / 3-influence blends, out-of-range bones and zero weights
//                      that the kernel must skip), 6 frames of animated palettes with one history reset. Each frame
//                      the output arena is read back and compared with the CPU kernel animation::skinning_lbs
//                      (kernel::launch CpuReference) BIT FOR BIT: current positions and normals from this frame's
//                      palette, previous positions from the previous palette (== last frame's, or this frame's on the
//                      first frame / after resetHistory). Also checks the per-instance GpuMesh rows (flags, stream
//                      addresses, index range of the base row) and the w channel (tangent handedness). Each kernel
//                      language built (Slang, GLSL).
//   --mode zero_alloc  64 frames (16 warm-up): 0 operator-new calls in setPose / beginFrame / the graph build with
//                      skin.lbs / the skin.lbs callback (validation off: the layer allocates).
//   --mode bench       1000 animated skinned characters (4,753 vertices, 32 bones each: 4.75M vertices, current +
//                      previous pose): per frame CPU palette evaluation + setPose + beginFrame, and the skin.lbs GPU
//                      time (timestamp queries); master plan target < 16 ms on the RTX 3090 (asserted on discrete GPUs;
//                      CPU devices such as Lavapipe run 100 characters and only report).
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_material_resolve_scene.hpp"
#include "test_rp_skinning_common.hpp"

#include <fuse/animation/skinning.hpp>
#include <fuse/animation/skinning_kernel.hpp>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/skinning/skinning_gpu.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <fuse/core/sanitizer.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#include <fuse/renderer/vk/test_layer_env.hpp>
#endif

namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

#if FUSE_TEST_GLOBAL_NEW_HOOKS
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {
constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::gpu_scene;
using namespace fuse::renderer::skinning;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace kernel = fuse::kernel;
namespace anim = fuse::animation;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr usize kStagingBytes = 32u * 1024u * 1024u;
u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_messages <= 20u) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                     data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

bool layerAvailable(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

BufferUsage bufferUsage(std::initializer_list<BufferUsage> bits) {
    u32 u = 0;
    for (const BufferUsage b : bits) {
        u |= static_cast<u32>(b);
    }
    return static_cast<BufferUsage>(u);
}

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    UploadQueue upload;
    Buffer staging{};
    u64 serial = 0;
    bool cpuDevice = false;
    f64 timestampPeriodNs = 1.0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
            allocator->destroyBuffer(staging);
        }
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

int setup(Context& ctx, bool validation) {
    if (validation) {
        if (!layerAvailable(kValidationLayer)) {
            std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n", kValidationLayer);
            return kSkip;
        }
        setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
        fuse::renderer::test::enableSyncValidationEnv();
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        setenv("VK_INSTANCE_LAYERS", "", 1);
    }
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_skinning";
    instanceDesc.enableValidation = validation;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    ctx.destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    if (createMessenger == nullptr && validation) {
        std::printf("SKIP: VK_EXT_debug_utils unavailable\n");
        return kSkip;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onMessage;
    if (createMessenger != nullptr) {
        createMessenger(vkInstance, &info, nullptr, &ctx.messenger);
    }
    ctx.device = VulkanDevice::create(*ctx.instance, VulkanDeviceDesc{});
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    if (!ctx.device->info().caps.bufferDeviceAddress || !ctx.device->info().caps.shaderInt64) {
        std::printf("SKIP: bufferDeviceAddress / shaderInt64 missing\n");
        return kSkip;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(static_cast<VkPhysicalDevice>(ctx.device->nativePhysicalDevice()), &props);
    ctx.cpuDevice = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    ctx.timestampPeriodNs = props.limits.timestampPeriod;
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_skinning.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

struct HostBuffer {
    Buffer buffer{};
    u8 queue = rg::kNoQueue;
    bool create(Context& ctx, u64 bytes) {
        BufferDesc d{};
        d.size = static_cast<usize>(bytes);
        d.usage = BufferUsage::TransferDst;
        d.memoryUsage = MemoryUsage::GpuToCpu;
        d.name = "rp_skinning.readback";
        return ctx.allocator->createBuffer(d, buffer) && buffer.mapped != nullptr;
    }
    void destroy(Context& ctx) {
        if (buffer.handle != nullptr) {
            ctx.allocator->destroyBuffer(buffer);
        }
        buffer = Buffer{};
    }
    rg::BufferRef import(rg::Graph& graph) {
        return graph.importBuffer(rg::ImportedBuffer{buffer.handle, buffer.desc.size, queue, &queue, "rp_skinning.readback"});
    }
};

struct CopyRecord {
    rg::BufferRef src;
    rg::BufferRef dst;
    u64 bytes = 0;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    const VkBufferCopy region{0, 0, c.bytes};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(c.src)),
                    static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

// --- scene ---------------------------------------------------------------------------------------------
struct SkinnedSet {
    GpuScene scene;
    SkinningGpu skin;
    std::vector<fuse::renderer::geometry::MeshletMesh> meshes;
    std::vector<sk_test::SkinMesh> skinMeshes; ///< per base mesh used for skinning
    std::vector<u32> skinMeshIds;
    std::vector<u32> instanceSkinMesh; ///< per skinned instance: index into skinMeshes
    std::vector<f32> phase;
    std::vector<InstanceHandle> handles;
};

bool buildSet(Context& ctx, SkinnedSet& s, SkinningKernelLanguage language, u32 characters, u32 bonesOverride,
              const mr_test::SourceMesh* characterMesh) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.instanceCapacity = characters + 16u;
    if (!s.scene.init(d) || !s.scene.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    s.scene.beginFrame(ctx.serial);
    std::vector<mr_test::SourceMesh> sources;
    if (characterMesh != nullptr) {
        sources.push_back(*characterMesh);
    } else {
        sources = {mr_test::uvSphere(16, 24, 1.f), mr_test::torus(24, 12, 1.f, 0.35f), mr_test::box()};
    }
    s.meshes.resize(sources.size());
    u64 restVerts = 0;
    for (u32 i = 0; i < sources.size(); ++i) {
        if (!mr_test::build(sources[i], s.meshes[i]) || s.scene.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
        restVerts += s.meshes[i].vertex_count();
    }
    const u32 skinBases = characterMesh != nullptr ? 1u : 2u; // sphere + torus skinned, the box stays rigid
    u64 outVerts = 0;
    for (u32 c = 0; c < characters; ++c) {
        outVerts += s.meshes[c % skinBases].vertex_count();
    }
    SkinningGpuDesc sd{};
    sd.device = ctx.device.get();
    sd.allocator = ctx.allocator.get();
    sd.upload = &ctx.upload;
    sd.maxRestVertices = static_cast<u32>(restVerts);
    sd.maxOutputVertices = static_cast<u32>(outVerts);
    sd.maxInstances = characters;
    sd.maxPaletteBones = characters * 32u;
    sd.language = language;
    if (!s.skin.init(sd)) {
        return false;
    }
    s.skin.attach(s.scene);
    for (u32 b = 0; b < skinBases; ++b) {
        const u32 bones = bonesOverride != 0u ? bonesOverride : (b == 0u ? 4u : 6u);
        s.skinMeshes.push_back(sk_test::makeSkinMesh(s.meshes[b], s.scene.mesh(b), bones));
        const sk_test::SkinMesh& sm = s.skinMeshes.back();
        SkinMeshDesc md{};
        md.restPositions = sm.positions.data();
        md.restNormals = sm.normals.data();
        md.handedness = sm.handedness.data();
        md.weights = sm.weights.data();
        md.vertexCount = sm.vertexCount;
        const u32 id = s.skin.addMesh(md);
        if (id == kInvalidSkin) {
            return false;
        }
        s.skinMeshIds.push_back(id);
    }
    // One rigid box instance (the static path must stay untouched), then the skinned characters on a grid.
    InstanceDesc rigid{};
    rigid.mesh = characterMesh != nullptr ? 0u : 2u;
    s.handles.push_back(s.scene.addInstance(rigid));
    const u32 side = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(characters))));
    for (u32 c = 0; c < characters; ++c) {
        const u32 b = c % skinBases;
        InstanceDesc id{};
        id.mesh = b;
        id.transform.rows[0][3] = 3.f * static_cast<f32>(c % side);
        id.transform.rows[2][3] = -3.f * static_cast<f32>(c / side);
        const InstanceHandle h = s.scene.addInstance(id);
        s.handles.push_back(h);
        SkinInstanceDesc si{};
        si.instance = h;
        si.mesh = s.skinMeshIds[b];
        si.baseMeshRow = b;
        si.boneCount = s.skinMeshes[b].bones;
        if (s.skin.addInstance(s.scene, si) != c) {
            return false;
        }
        s.instanceSkinMesh.push_back(b);
        s.phase.push_back(0.37f * static_cast<f32>(c));
    }
    const GpuSceneCommitStats stats = s.scene.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

void destroySet(SkinnedSet& s) {
    s.skin.destroy();
    s.scene.destroy();
}

/// Poses every character for time step `frame` (the scratch palette is reused: no allocation).
void poseAll(SkinnedSet& s, u32 frame, std::vector<f32>& scratch) {
    for (u32 c = 0; c < s.instanceSkinMesh.size(); ++c) {
        const sk_test::SkinMesh& sm = s.skinMeshes[s.instanceSkinMesh[c]];
        sk_test::palette(sm, 0.4f * static_cast<f32>(frame), s.phase[c], 0.35f, scratch.data());
        s.skin.setPose(c, scratch.data(), sm.bones);
    }
}

// --- parity ----------------------------------------------------------------------------------------------
u32 bitMismatches(const f32* gpu, u32 strideGpu, const anim::vec3* cpu, u32 n, u32& worstUlp) {
    u32 bad = 0;
    for (u32 v = 0; v < n; ++v) {
        const f32 c[3] = {cpu[v].x, cpu[v].y, cpu[v].z};
        for (u32 a = 0; a < 3u; ++a) {
            u32 gb = 0;
            u32 cb = 0;
            std::memcpy(&gb, &gpu[v * strideGpu + a], 4u);
            std::memcpy(&cb, &c[a], 4u);
            if (gb != cb) {
                ++bad;
                const u32 d = gb > cb ? gb - cb : cb - gb;
                worstUlp = std::max(worstUlp, d);
            }
        }
    }
    return bad;
}

int runParity(Context& ctx) {
    const SkinningKernelLanguage languages[2] = {SkinningKernelLanguage::Slang, SkinningKernelLanguage::Glsl};
    u32 ran = 0;
    constexpr u32 kCharacters = 7;
    constexpr u32 kFrames = 6;
    for (const SkinningKernelLanguage language : languages) {
        SkinnedSet s;
        if (!buildSet(ctx, s, language, kCharacters, 0u, nullptr)) {
            const bool notBuilt = !s.skin.valid() && s.scene.gpuEnabled();
            destroySet(s);
            if (notBuilt) {
                continue;
            }
            std::fprintf(stderr, "FAIL: scene / skinning init\n");
            return 1;
        }
        ++ran;
        // Rows: flags, streams and the base row's index range.
        for (u32 c = 0; c < kCharacters; ++c) {
            const GpuMesh& row = s.scene.mesh(s.skin.meshRow(c));
            const GpuMesh& base = s.scene.mesh(s.instanceSkinMesh[c]);
            const u64 out = s.skin.outputBuffer().deviceAddress + s.skin.outputOffset(c);
            expect((row.flags & (kMeshFloatPositions | kMeshFloatNormals | kMeshSkinned)) ==
                       (kMeshFloatPositions | kMeshFloatNormals | kMeshSkinned),
                   "skinned row flags");
            expect(row.positions == out && row.normals == out + 32ull * row.vertexCount, "skinned row stream addresses");
            expect(row.firstIndex == base.firstIndex && row.indexCount == base.indexCount && row.uvs == base.uvs &&
                       row.tangents == base.tangents && row.vertexCount == base.vertexCount,
                   "skinned row keeps the base row's index range and attribute streams");
            expect(s.scene.instance(s.handles[c + 1u].slot).mesh == s.skin.meshRow(c), "instance points at its skinned row");
        }
        expect(s.scene.mesh(2).flags == 0u && s.scene.instance(s.handles[0].slot).mesh == 2u, "rigid row untouched");
        const u64 bytes = s.skin.outputBuffer().desc.size;
        HostBuffer rb;
        if (!rb.create(ctx, bytes)) {
            std::fprintf(stderr, "FAIL: readback\n");
            return 1;
        }
        std::vector<f32> scratch(32u * 16u);
        std::vector<std::vector<f32>> lastPalette(kCharacters);
        rg::Graph graph;
        u32 badPos = 0, badPrev = 0, badNrm = 0, badW = 0, badHistory = 0, worstUlp = 0;
        u64 checked = 0;
        for (u32 frame = 0; frame < kFrames; ++frame) {
            ++ctx.serial;
            s.scene.beginFrame(ctx.serial);
            poseAll(s, frame, scratch);
            if (frame == 3u) {
                s.skin.resetHistory(5u); // teleport: no skinned motion this frame
            }
            const GpuSceneCommitStats stats = s.scene.commit();
            ctx.upload.flush();
            // Frame 4 reuses frame 3's ring slot (serial + 3): the previous palettes are uploaded instead of read in place.
            expect(s.skin.beginFrame(frame == 4u ? ctx.serial + 2u : ctx.serial), "beginFrame");
            graph.reset();
            const GpuSceneGraphRefs refs = s.scene.importInto(graph);
            s.skin.addSkinning(graph, refs);
            CopyRecord copy{};
            copy.src = refs.dynamicGeometry;
            copy.dst = rb.import(graph);
            copy.bytes = bytes;
            graph.addPass("readback.copy", &recordCopy, &copy)
                .use(copy.src, rg::Access::TransferSrc, rg::BufferRange{0, bytes})
                .use(copy.dst, rg::Access::TransferDst, rg::BufferRange{0, bytes});
            graph.addPass("readback.host", nullptr, nullptr).use(copy.dst, rg::Access::HostRead);
            const rg::ExecuteResult result = ctx.executor->execute(graph);
            const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
            s.scene.collectRetired(ctx.serial);
            if (!stats.ok || !result.ok || !waited) {
                std::fprintf(stderr, "FAIL: frame %u execute\n", frame);
                return 1;
            }
            expect(s.skin.stats().dispatches == 1u, "one skin.lbs pass per frame");
            const f32* gpu = static_cast<const f32*>(rb.buffer.mapped);
            for (u32 c = 0; c < kCharacters; ++c) {
                const sk_test::SkinMesh& sm = s.skinMeshes[s.instanceSkinMesh[c]];
                const u32 n = sm.vertexCount;
                const u32 bones = s.skin.boneCount(c);
                // History: previous palette == last frame's current one (this frame's on frame 0 / after the reset).
                const std::vector<f32> cur(s.skin.currentPalette(c), s.skin.currentPalette(c) + bones * 16u);
                const std::vector<f32>& expectPrev = frame == 0u || (frame == 3u && c == 5u) ? cur : lastPalette[c];
                badHistory += std::memcmp(expectPrev.data(), s.skin.previousPalette(c), bones * 16u * sizeof(f32)) != 0 ? 1u : 0u;
                lastPalette[c] = cur;
                // CPU kernel "skinning_lbs" with both palettes.
                anim::SkinningInput in;
                in.rest_positions.resize(n);
                in.rest_normals.resize(n);
                in.weights.resize(n);
                for (u32 v = 0; v < n; ++v) {
                    in.rest_positions[v] = {sm.positions[v * 3u], sm.positions[v * 3u + 1u], sm.positions[v * 3u + 2u], 0.f};
                    in.rest_normals[v] = {sm.normals[v * 3u], sm.normals[v * 3u + 1u], sm.normals[v * 3u + 2u], 0.f};
                    static_assert(sizeof(anim::SkinningWeights) == sizeof(SkinVertexWeights), "weights layout");
                    std::memcpy(&in.weights[v], &sm.weights[v], sizeof(SkinVertexWeights));
                }
                auto runCpu = [&](const f32* pal, anim::SkinningOutput& out) {
                    in.bone_transforms.resize(bones);
                    for (u32 b = 0; b < bones; ++b) {
                        std::memcpy(in.bone_transforms[b].data.data(), pal + b * 16u, 64u);
                    }
                    out.positions.resize(n);
                    out.normals.resize(n);
                    anim::skinning_kernel::Params p = anim::skinning_kernel::make_params(in, out);
                    expect(kernel::launch(kernel::Backend::CpuReference, anim::skinning_kernel::make_launch(p),
                                          anim::skinning_kernel::Kernel{}, p)
                               .ok,
                           "CPU skinning_lbs");
                };
                anim::SkinningOutput curOut;
                anim::SkinningOutput prevOut;
                runCpu(s.skin.currentPalette(c), curOut);
                runCpu(s.skin.previousPalette(c), prevOut);
                const f32* o = gpu + s.skin.outputOffset(c) / sizeof(f32);
                badPos += bitMismatches(o, 4u, curOut.positions.data(), n, worstUlp);
                badPrev += bitMismatches(o + 4u * n, 4u, prevOut.positions.data(), n, worstUlp);
                badNrm += bitMismatches(o + 8u * n, 4u, curOut.normals.data(), n, worstUlp);
                for (u32 v = 0; v < n; ++v) {
                    const f32 hand = sm.handedness[v] < 0.f ? -1.f : 1.f;
                    badW += (o[v * 4u + 3u] != hand || o[(n + v) * 4u + 3u] != hand || o[(2u * n + v) * 4u + 3u] != 0.f) ? 1u : 0u;
                }
                checked += n;
            }
            std::printf("  [%s] frame %u: %u skinned instances, %u vertices x 2 poses; mismatching values: current %u, "
                        "previous %u, normals %u (worst %u ulp)\n",
                        s.skin.kernelLanguage(), frame, kCharacters, s.skin.stats().vertices, badPos, badPrev, badNrm, worstUlp);
        }
        std::printf("[%s] %u frames, %llu vertex checks: GPU skin.lbs == CPU skinning_lbs bit for bit: %s (current %u, "
                    "previous %u, normals %u, w %u, palette history %u mismatches)\n",
                    s.skin.kernelLanguage(), kFrames, static_cast<unsigned long long>(checked),
                    badPos + badPrev + badNrm == 0u ? "yes" : "NO", badPos, badPrev, badNrm, badW, badHistory);
        expect(badPos == 0u, "current positions bit-identical to skinning_lbs");
        expect(badPrev == 0u, "previous positions bit-identical to skinning_lbs on the previous palette");
        expect(badNrm == 0u, "normals bit-identical to skinning_lbs");
        expect(badW == 0u, "w = handedness (positions), 0 (normals)");
        expect(badHistory == 0u, "previous palette == last frame's (== current on frame 0 / after resetHistory)");
        rb.destroy(ctx);
        destroySet(s);
    }
    if (ran == 0u) {
        std::printf("SKIP: no skinning kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------------
thread_local bool t_inPass = false;
void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "skin.", 5) == 0) {
        t_inPass = true;
        t_count = true;
    }
}
void hookEnd(const rg::PassContext&, const char*, void*) {
    if (t_inPass) {
        t_inPass = false;
        t_count = false;
    }
}

int runZeroAlloc(Context& ctx) {
    SkinnedSet s;
    if (!buildSet(ctx, s, SkinningKernelLanguage::Auto, 24u, 0u, nullptr)) {
        const bool notBuilt = !s.skin.valid() && s.scene.gpuEnabled();
        destroySet(s);
        if (notBuilt) {
            std::printf("SKIP: no skinning kernel built\n");
            return kSkip;
        }
        std::fprintf(stderr, "FAIL: scene / skinning init\n");
        return 1;
    }
    std::vector<f32> scratch(32u * 16u);
    rg::Graph graph;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 64;
    unsigned long long frameSide = 0, build = 0, callbacks = 0;
    ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = frame >= kWarmup;
        ++ctx.serial;
        s.scene.beginFrame(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        poseAll(s, frame, scratch);
        const bool began = s.skin.beginFrame(ctx.serial);
        t_count = false;
        const unsigned long long side = t_allocations;
        s.scene.commit();
        ctx.upload.flush();
        graph.reset();
        t_allocations = 0;
        t_count = measure;
        const GpuSceneGraphRefs refs = s.scene.importInto(graph);
        s.skin.addSkinning(graph, refs);
        graph.addPass("consume", nullptr, nullptr).use(refs.dynamicGeometry, rg::Access::StorageRead, {}, rg::kStageCompute).neverCull();
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        s.scene.collectRetired(ctx.serial);
        expect(began && result.ok && waited, "frame ok");
        if (measure) {
            frameSide += side + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    std::printf("zero_alloc: %u steady-state frames, %u skinned instances: setPose + beginFrame + skin.lbs callback: %llu "
                "operator-new calls (callback %llu); graph build (scene import + skin.lbs): %llu\n",
                kTotal - kWarmup, s.skin.instanceCount(), frameSide, callbacks, build);
    expect(frameSide == 0u, "skinning side makes no steady-state heap allocations");
    expect(build == 0u, "graph build with skin.lbs makes no steady-state heap allocations");
    destroySet(s);
    return 0;
}

// --- bench -----------------------------------------------------------------------------------------------
struct Timestamps {
    VkQueryPool pool = VK_NULL_HANDLE;
};
void benchBegin(const rg::PassContext& pc, const char* name, void* user) {
    if (name != nullptr && std::strcmp(name, "skin.lbs") == 0) {
        const Timestamps& t = *static_cast<const Timestamps*>(user);
        VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
        vkCmdResetQueryPool(cmd, t.pool, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, t.pool, 0);
    }
}
void benchEnd(const rg::PassContext& pc, const char* name, void* user) {
    if (name != nullptr && std::strcmp(name, "skin.lbs") == 0) {
        const Timestamps& t = *static_cast<const Timestamps*>(user);
        vkCmdWriteTimestamp(static_cast<VkCommandBuffer>(pc.commandBuffer), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, t.pool, 1);
    }
}

int runBench(Context& ctx) {
    const u32 characters = ctx.cpuDevice ? 100u : 1000u;
    constexpr u32 kBones = 32;
    const mr_test::SourceMesh body = mr_test::uvSphere(48, 96, 1.f); // 49 x 97 = 4,753 vertices
    SkinnedSet s;
    if (!buildSet(ctx, s, SkinningKernelLanguage::Auto, characters, kBones, &body)) {
        const bool notBuilt = !s.skin.valid() && s.scene.gpuEnabled();
        destroySet(s);
        if (notBuilt) {
            std::printf("SKIP: no skinning kernel built\n");
            return kSkip;
        }
        std::fprintf(stderr, "FAIL: scene / skinning init\n");
        return 1;
    }
    Timestamps ts;
    VkQueryPoolCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2;
    if (vkCreateQueryPool(ctx.vkDevice, &qi, nullptr, &ts.pool) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: query pool\n");
        return 1;
    }
    ctx.executor->setPassHooks(rg::PassHooks{&benchBegin, &benchEnd, &ts});
    std::vector<f32> palettes(static_cast<usize>(characters) * kBones * 16u);
    rg::Graph graph;
    const u32 frames = ctx.cpuDevice ? 6u : 60u;
    std::vector<f64> gpuMs;
    std::vector<f64> animMs;
    std::vector<f64> cpuMs;
    for (u32 frame = 0; frame < frames; ++frame) {
        ++ctx.serial;
        s.scene.beginFrame(ctx.serial);
        const auto ta = std::chrono::steady_clock::now();
        for (u32 c = 0; c < characters; ++c) { // the test's "animation": a procedural 32-joint chain per character (f64)
            sk_test::palette(s.skinMeshes[0], 0.4f * static_cast<f32>(frame), s.phase[c], 0.35f, &palettes[c * kBones * 16u]);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (u32 c = 0; c < characters; ++c) {
            s.skin.setPose(c, &palettes[c * kBones * 16u], kBones);
        }
        s.skin.beginFrame(ctx.serial);
        const auto t1 = std::chrono::steady_clock::now();
        s.scene.commit();
        ctx.upload.flush();
        graph.reset();
        const GpuSceneGraphRefs refs = s.scene.importInto(graph);
        s.skin.addSkinning(graph, refs);
        graph.addPass("consume", nullptr, nullptr).use(refs.dynamicGeometry, rg::Access::StorageRead, {}, rg::kStageCompute).neverCull();
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        s.scene.collectRetired(ctx.serial);
        if (!result.ok || !waited) {
            std::fprintf(stderr, "FAIL: bench frame %u\n", frame);
            return 1;
        }
        u64 stamps[2] = {0, 0};
        if (vkGetQueryPoolResults(ctx.vkDevice, ts.pool, 0, 2, sizeof(stamps), stamps, sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS &&
            frame >= 2u) {
            gpuMs.push_back(static_cast<f64>(stamps[1] - stamps[0]) * ctx.timestampPeriodNs * 1e-6);
            cpuMs.push_back(std::chrono::duration<f64, std::milli>(t1 - t0).count());
            animMs.push_back(std::chrono::duration<f64, std::milli>(t0 - ta).count());
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    vkDestroyQueryPool(ctx.vkDevice, ts.pool, nullptr);
    auto median = [](std::vector<f64> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2u];
    };
    const f64 g = median(gpuMs);
    const f64 c = median(cpuMs);
    const f64 a = median(animMs);
    const f64 vertices = static_cast<f64>(s.skin.stats().vertices);
    std::printf("bench: %s, %u skinned characters x %u vertices x %u bones (%.2fM vertices, current + previous pose + normals)\n"
                "  skin.lbs GPU (timestamps) median %.3f ms (%.0f Mvert/s)\n"
                "  CPU setPose + beginFrame (palette history + ring upload) median %.3f ms\n"
                "  test-side pose evaluation (procedural f64 joint chain, one thread; not renderer work) median %.3f ms\n"
                "  total %.3f ms (master plan target < 16 ms for 1000 animated characters)\n",
                ctx.device->info().deviceName.c_str(), characters, s.skinMeshes[0].vertexCount, kBones, vertices * 1e-6, g,
                g > 0.0 ? vertices / (g * 1e3) : 0.0, c, a, g + c + a);
    if (!ctx.cpuDevice) {
        expect(g + c + a < 16.0, "1000 animated skinned characters: pose evaluation + upload + GPU skinning < 16 ms");
    } else {
        std::printf("  (CPU device: %u characters, timing reported, not asserted)\n", characters);
    }
    destroySet(s);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        }
    }
    const bool validation = mode == "parity";
    int rc = 0;
    {
        Context ctx;
        rc = setup(ctx, validation);
        if (rc == 0) {
            if (mode == "parity") {
                rc = runParity(ctx);
            } else if (mode == "zero_alloc") {
                rc = runZeroAlloc(ctx);
            } else if (mode == "bench") {
                rc = runBench(ctx);
            } else {
                std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
                rc = 2;
            }
        }
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (g_messages != 0u) {
        std::fprintf(stderr, "FAIL: %u validation messages\n", g_messages);
        return 1;
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "FAIL (%d failures)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}

#endif
