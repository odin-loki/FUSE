// WP-6.0 acceleration structures (see include/fuse/renderer/rt/acceleration_structures.hpp).
#include <fuse/renderer/rt/acceleration_structures.hpp>

#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#include "rt_spv.h"
#endif

#if defined(FUSE_VULKAN_BACKEND) && defined(VK_KHR_acceleration_structure)
#define FUSE_RT_VULKAN 1
#endif

namespace fuse::renderer::rt {

using gpu_scene::GpuSceneTable;
using gpu_scene::kInvalidIndex;

namespace {

constexpr u64 kVertexStride = 12u; ///< f32 x 3
[[maybe_unused]] constexpr u32 kPushBytes = 64u; ///< largest kernel push block (RtInstancesPush)

[[maybe_unused]] u64 alignUp(u64 value, u64 alignment) {
    return alignment <= 1u ? value : (value + alignment - 1u) / alignment * alignment;
}

[[maybe_unused]] BufferUsage usageOf(u32 bits) {
    return static_cast<BufferUsage>(bits);
}

[[maybe_unused]] constexpr u32 kUsageAsStorage = static_cast<u32>(BufferUsage::AccelerationStructureStorage) |
                                static_cast<u32>(BufferUsage::ShaderDeviceAddress);
[[maybe_unused]] constexpr u32 kUsageScratch = static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress);
[[maybe_unused]] constexpr u32 kUsageArena = static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                            static_cast<u32>(BufferUsage::AccelerationStructureBuildInput);
[[maybe_unused]] constexpr u32 kUsageInstances = static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                static_cast<u32>(BufferUsage::AccelerationStructureBuildInput) |
                                static_cast<u32>(BufferUsage::TransferDst) | static_cast<u32>(BufferUsage::TransferSrc);
[[maybe_unused]] constexpr u32 kUsageTable = static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                            static_cast<u32>(BufferUsage::TransferDst) | static_cast<u32>(BufferUsage::TransferSrc);

[[maybe_unused]] rg::BufferRef importRef(rg::Graph& graph, const Buffer& buffer, const char* name) {
    if (buffer.handle == nullptr) {
        return rg::BufferRef{};
    }
    return graph.importBuffer(rg::ImportedBuffer{buffer.handle, static_cast<u64>(buffer.desc.size),
                                                 static_cast<u8>(rg::QueueClass::Graphics), nullptr, name});
}

} // namespace

#if defined(FUSE_RT_VULKAN)
struct AccelerationStructures::Impl {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkCreateAccelerationStructureKHR create = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR address = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR cmdWriteProperties = nullptr;
    PFN_vkCmdCopyAccelerationStructureKHR cmdCopy = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline decode = VK_NULL_HANDLE;
    VkPipeline instances = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    u32 queryCapacity = 0;
    // Record-time scratch (sized in commit(), reused every frame).
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers;
    std::vector<RtDecodePush> decodePush;
};
#else
struct AccelerationStructures::Impl {};
#endif

AccelerationStructures::AccelerationStructures() = default;

AccelerationStructures::~AccelerationStructures() {
    destroy();
}

const BlasInfo& AccelerationStructures::blas(u32 mesh) const {
    return mesh < m_blas.size() ? m_blas[mesh].info : m_noBlas;
}

bool AccelerationStructures::setMeshOptions(u32 mesh, const RtMeshOptions& options) {
    if (mesh >= kRtMaxInstances) {
        return false;
    }
    if (mesh >= m_options.size()) {
        m_options.resize(static_cast<usize>(mesh) + 1u);
    }
    if (mesh < m_blas.size() && m_blas[mesh].info.state != BlasState::None) {
        return false; // already built with the old options
    }
    m_options[mesh] = options;
    return true;
}

bool AccelerationStructures::deformMesh(u32 mesh, const Buffer& positions, u64 offset) {
    if (!m_ready || m_desc.scene == nullptr || mesh >= m_desc.scene->meshCount() || positions.handle == nullptr ||
        positions.deviceAddress == 0u || mesh >= m_options.size() || !m_options[mesh].deformable) {
        return false;
    }
    const GpuMesh& gm = m_desc.scene->mesh(mesh);
    const u64 bytes = static_cast<u64>(gm.vertexCount) * kVertexStride;
    if (offset + bytes > static_cast<u64>(positions.desc.size) || (offset & 3u) != 0u) {
        return false;
    }
    if (mesh >= m_blas.size()) {
        m_blas.resize(static_cast<usize>(mesh) + 1u);
    }
    Blas& b = m_blas[mesh];
    b.deformBuffer = positions.handle;
    b.deformAddress = positions.deviceAddress + offset;
    b.deformSize = static_cast<u64>(positions.desc.size);
    b.deformPending = true;
    return true;
}

RtMemoryStats AccelerationStructures::memory() const {
    RtMemoryStats s{};
    for (const Blas& b : m_blas) {
        if (b.handle == nullptr) {
            continue;
        }
        ++s.blasCount;
        s.blasBytes += b.info.size;
        s.blasBuildBytes += b.info.buildSize;
        s.compactedCount += b.info.state == BlasState::Compacted ? 1u : 0u;
    }
    s.tlasBytes = m_tlasBuffer.desc.size;
    s.scratchBytes = m_scratch.desc.size;
    s.instanceBytes = m_instanceBuffer.desc.size;
    return s;
}

void AccelerationStructures::beginFrame(u64 frameSerial) {
    m_frameSerial = frameSerial;
}

u32 AccelerationStructures::collectRetired(u64 completedSerial) {
    m_completedSerial = std::max(m_completedSerial, completedSerial);
    u32 destroyed = 0;
    usize keep = 0;
    for (usize i = 0; i < m_retired.size(); ++i) {
        Retired& r = m_retired[i];
        if (r.serial <= completedSerial) {
            destroyAs(r.handle, r.buffer);
            ++destroyed;
        } else {
            m_retired[keep++] = r;
        }
    }
    m_retired.resize(keep);
    return destroyed;
}

void AccelerationStructures::retire(void* handle, Buffer& buffer) {
    if (handle != nullptr || buffer.handle != nullptr) {
        m_retired.push_back(Retired{handle, buffer, m_frameSerial});
    }
    buffer = Buffer{};
}

#if defined(FUSE_RT_VULKAN)

// --- init / destroy ----------------------------------------------------------------------------------

bool AccelerationStructures::init(const AccelerationStructuresDesc& desc) {
    destroy();
    m_desc = desc;
    m_caps = queryRtCapabilities(desc.device);
    if (!m_caps.usable) {
        m_reason = m_caps.reason;
        return false;
    }
    if (desc.allocator == nullptr || desc.upload == nullptr || desc.scene == nullptr || !desc.scene->gpuEnabled()) {
        m_reason = "AccelerationStructuresDesc needs an allocator, an upload queue and a GPU-enabled scene";
        return false;
    }
    m_scratchAlignment = std::max<u32>(m_caps.minScratchAlignment, 16u);
    m_impl = std::make_unique<Impl>();
    Impl& im = *m_impl;
    im.device = static_cast<VkDevice>(desc.device->nativeHandle());
    im.create = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(im.device, "vkCreateAccelerationStructureKHR"));
    im.destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(im.device, "vkDestroyAccelerationStructureKHR"));
    im.sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(im.device, "vkGetAccelerationStructureBuildSizesKHR"));
    im.address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(im.device, "vkGetAccelerationStructureDeviceAddressKHR"));
    im.cmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(im.device, "vkCmdBuildAccelerationStructuresKHR"));
    im.cmdWriteProperties = reinterpret_cast<PFN_vkCmdWriteAccelerationStructuresPropertiesKHR>(
        vkGetDeviceProcAddr(im.device, "vkCmdWriteAccelerationStructuresPropertiesKHR"));
    im.cmdCopy = reinterpret_cast<PFN_vkCmdCopyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(im.device, "vkCmdCopyAccelerationStructureKHR"));
    if (im.create == nullptr || im.destroy == nullptr || im.sizes == nullptr || im.address == nullptr ||
        im.cmdBuild == nullptr || im.cmdWriteProperties == nullptr || im.cmdCopy == nullptr) {
        m_reason = "acceleration-structure entry points missing";
        m_impl.reset();
        return false;
    }
    if (!createKernels()) {
        m_reason = "no rt kernels built (rt_decode needs Slang or glslangValidator)";
        destroyKernels();
        m_impl.reset();
        return false;
    }
    m_gpuPacking = desc.packing != RtInstancePacking::Cpu && im.instances != VK_NULL_HANDLE;
    if (desc.compaction) {
        im.queryCapacity = std::max<u32>(desc.meshCapacity, 16u);
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qi.queryCount = im.queryCapacity;
        if (vkCreateQueryPool(im.device, &qi, nullptr, &im.queries) != VK_SUCCESS) {
            im.queries = VK_NULL_HANDLE;
            im.queryCapacity = 0;
        }
    }
    const u32 meshes = std::max<u32>(desc.meshCapacity, 1u);
    m_blas.reserve(meshes);
    m_options.reserve(meshes);
    m_addressMirror.reserve(meshes);
    m_blasRefs.reserve(meshes);
    m_ops.reserve(64);
    m_retired.reserve(64);
    im.geometries.reserve(64);
    im.infos.reserve(64);
    im.ranges.reserve(64);
    im.rangePointers.reserve(64);
    im.decodePush.reserve(64);
    if (!ensureTlas(std::max<u32>(desc.instanceCapacity, 1u))) {
        m_reason = "TLAS / instance buffer creation failed";
        destroy();
        return false;
    }
    m_ready = true;
    m_reason = "ok";
    return true;
}

void AccelerationStructures::destroy() {
    if (m_impl == nullptr) {
        m_ready = false;
        return;
    }
    for (Blas& b : m_blas) {
        destroyAs(b.handle, b.buffer);
        b = Blas{};
    }
    for (Retired& r : m_retired) {
        destroyAs(r.handle, r.buffer);
    }
    m_retired.clear();
    destroyAs(m_tlas, m_tlasBuffer);
    m_tlas = nullptr;
    void* none = nullptr;
    destroyAs(none, m_instanceBuffer);
    destroyAs(none, m_addressBuffer);
    destroyAs(none, m_decodeArena);
    destroyAs(none, m_scratch);
    if (m_impl->queries != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_impl->device, m_impl->queries, nullptr);
    }
    destroyKernels();
    m_impl.reset();
    m_blas.clear();
    m_addressMirror.clear();
    m_blasRefs.clear();
    m_ops.clear();
    m_tlasCapacity = 0;
    m_tlasCount = 0;
    m_tlasBuilt = false;
    m_tlasAddress = 0;
    m_addressCapacity = 0;
    m_ready = false;
    m_reason = "destroyed";
    m_language = "none";
}

void AccelerationStructures::destroyAs(void* handle, Buffer& buffer) {
    if (m_impl != nullptr && handle != nullptr) {
        m_impl->destroy(m_impl->device, static_cast<VkAccelerationStructureKHR>(handle), nullptr);
    }
    if (buffer.handle != nullptr && m_desc.allocator != nullptr) {
        m_desc.allocator->destroyBuffer(buffer);
    }
    buffer = Buffer{};
}

bool AccelerationStructures::createKernels() {
    Impl& im = *m_impl;
    const u32* decode = nullptr;
    usize decodeBytes = 0;
    const u32* instances = nullptr;
    usize instancesBytes = 0;
    const char* language = "none";
#if defined(FUSE_RT_SLANG)
    if (m_desc.language != RtKernelLanguage::Glsl) {
        decode = kRtDecodeSlangSpv;
        decodeBytes = sizeof(kRtDecodeSlangSpv);
        instances = kRtInstancesSlangSpv;
        instancesBytes = sizeof(kRtInstancesSlangSpv);
        language = "slang";
    }
#endif
#if defined(FUSE_RT_GLSL)
    if (decode == nullptr && m_desc.language != RtKernelLanguage::Slang) {
        decode = kRtDecodeGlslSpv;
        decodeBytes = sizeof(kRtDecodeGlslSpv);
        instances = kRtInstancesGlslSpv;
        instancesBytes = sizeof(kRtInstancesGlslSpv);
        language = "glsl";
    }
#endif
    if (decode == nullptr) {
        return false;
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushBytes};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(im.device, &layoutInfo, nullptr, &im.layout) != VK_SUCCESS) {
        im.layout = VK_NULL_HANDLE;
        return false;
    }
    // Descriptor-free pipelines follow the frame's bindless backend (see AccelerationStructuresDesc::bindless).
    const BindlessDescriptors* heap =
        m_desc.bindless != nullptr ? m_desc.bindless : (m_desc.scene != nullptr ? m_desc.scene->desc().bindless : nullptr);
    const VkPipelineCreateFlags createFlags = heap != nullptr ? static_cast<VkPipelineCreateFlags>(heap->pipelineCreateFlags()) : 0u;
    auto make = [&](const u32* code, usize bytes, VkPipeline& out) {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = bytes;
        mi.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(im.device, &mi, nullptr, &module) != VK_SUCCESS) {
            return false;
        }
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.flags = createFlags;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = im.layout;
        const bool ok = vkCreateComputePipelines(im.device, VK_NULL_HANDLE, 1, &info, nullptr, &out) == VK_SUCCESS;
        vkDestroyShaderModule(im.device, module, nullptr);
        if (!ok) {
            out = VK_NULL_HANDLE;
        }
        return ok;
    };
    if (!make(decode, decodeBytes, im.decode)) {
        return false;
    }
    if (instances != nullptr) {
        make(instances, instancesBytes, im.instances); // optional: CPU packing without it
    }
    m_language = language;
    return true;
}

void AccelerationStructures::destroyKernels() {
    if (m_impl == nullptr) {
        return;
    }
    Impl& im = *m_impl;
    if (im.decode != VK_NULL_HANDLE) {
        vkDestroyPipeline(im.device, im.decode, nullptr);
    }
    if (im.instances != VK_NULL_HANDLE) {
        vkDestroyPipeline(im.device, im.instances, nullptr);
    }
    if (im.layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(im.device, im.layout, nullptr);
    }
    im.decode = VK_NULL_HANDLE;
    im.instances = VK_NULL_HANDLE;
    im.layout = VK_NULL_HANDLE;
}

// --- buffers and objects ---------------------------------------------------------------------------

bool AccelerationStructures::createAsBuffer(Buffer& out, u64 bytes, const char* name) {
    BufferDesc d{};
    d.size = static_cast<usize>(std::max<u64>(bytes, 256u));
    d.usage = usageOf(kUsageAsStorage);
    d.memoryUsage = MemoryUsage::GpuOnly;
    d.name = name;
    out = Buffer{};
    if (!m_desc.allocator->createBuffer(d, out) || out.deviceAddress == 0u) {
        if (out.handle != nullptr) {
            m_desc.allocator->destroyBuffer(out);
        }
        out = Buffer{};
        return false;
    }
    return true;
}

bool AccelerationStructures::ensureBuffer(Buffer& buffer, u64 bytes, u32 usage, const char* name, bool mapped) {
    if (buffer.handle != nullptr && static_cast<u64>(buffer.desc.size) >= bytes) {
        return true;
    }
    const u64 capacity = std::max<u64>(std::max<u64>(bytes, 256u), static_cast<u64>(buffer.desc.size) * 2u);
    BufferDesc d{};
    d.size = static_cast<usize>(capacity);
    d.usage = usageOf(usage);
    d.memoryUsage = mapped ? MemoryUsage::CpuToGpu : MemoryUsage::GpuOnly;
    d.name = name;
    Buffer fresh{};
    if (!m_desc.allocator->createBuffer(d, fresh) || fresh.deviceAddress == 0u) {
        if (fresh.handle != nullptr) {
            m_desc.allocator->destroyBuffer(fresh);
        }
        return false;
    }
    retire(nullptr, buffer);
    buffer = fresh;
    return true;
}

bool AccelerationStructures::createBlasObject(u32 mesh, Blas& blas) {
    Impl& im = *m_impl;
    const GpuMesh& gm = m_desc.scene->mesh(mesh);
    const bool deformable = mesh < m_options.size() && m_options[mesh].deformable;
    blas.flags = deformable ? (VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                               VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR)
                            : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (!deformable && m_desc.compaction && im.queries != VK_NULL_HANDLE && mesh < im.queryCapacity) {
        blas.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    }
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geometry.geometry.triangles.vertexStride = kVertexStride;
    geometry.geometry.triangles.maxVertex = gm.vertexCount - 1u;
    geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = blas.flags;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    const u32 primitives = gm.indexCount / 3u;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    im.sizes(im.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
    if (sizes.accelerationStructureSize == 0u || !createAsBuffer(blas.buffer, sizes.accelerationStructureSize, "rt.blas")) {
        return false;
    }
    VkAccelerationStructureCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = static_cast<VkBuffer>(blas.buffer.handle);
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    if (im.create(im.device, &ci, nullptr, &handle) != VK_SUCCESS) {
        m_desc.allocator->destroyBuffer(blas.buffer);
        blas.buffer = Buffer{};
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai{};
    ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    ai.accelerationStructure = handle;
    blas.handle = handle;
    blas.buildScratch = sizes.buildScratchSize;
    blas.updateScratch = sizes.updateScratchSize;
    blas.info.address = im.address(im.device, &ai);
    blas.info.size = sizes.accelerationStructureSize;
    blas.info.buildSize = sizes.accelerationStructureSize;
    blas.info.compactedSize = 0;
    blas.info.triangles = primitives;
    blas.info.vertices = gm.vertexCount;
    blas.info.deformable = deformable;
    blas.info.updates = 0;
    return blas.info.address != 0u;
}

bool AccelerationStructures::ensureTlas(u32 instanceCount) {
    if (m_tlas != nullptr && instanceCount <= m_tlasCapacity) {
        return true;
    }
    Impl& im = *m_impl;
    u32 capacity = std::max<u32>(m_tlasCapacity, std::max<u32>(m_desc.instanceCapacity, 16u));
    while (capacity < instanceCount) {
        capacity *= 2u;
    }
    capacity = std::min(capacity, kRtMaxInstances);
    if (instanceCount > capacity) {
        return false;
    }
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    im.sizes(im.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacity, &sizes);
    Buffer buffer{};
    if (sizes.accelerationStructureSize == 0u || !createAsBuffer(buffer, sizes.accelerationStructureSize, "rt.tlas")) {
        return false;
    }
    VkAccelerationStructureCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = static_cast<VkBuffer>(buffer.handle);
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    if (im.create(im.device, &ci, nullptr, &handle) != VK_SUCCESS) {
        m_desc.allocator->destroyBuffer(buffer);
        return false;
    }
    if (!ensureBuffer(m_instanceBuffer, static_cast<u64>(capacity) * sizeof(AsInstance), kUsageInstances, "rt.instances")) {
        im.destroy(im.device, handle, nullptr);
        m_desc.allocator->destroyBuffer(buffer);
        return false;
    }
    retire(m_tlas, m_tlasBuffer);
    m_tlas = handle;
    m_tlasBuffer = buffer;
    VkAccelerationStructureDeviceAddressInfoKHR ai{};
    ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    ai.accelerationStructure = handle;
    m_tlasAddress = im.address(im.device, &ai);
    m_tlasBuildScratch = sizes.buildScratchSize;
    m_tlasUpdateScratch = sizes.updateScratchSize;
    m_tlasCapacity = capacity;
    m_tlasBuilt = false; // a new object always starts with a full build
    m_cpuInstances.resize(capacity);
    m_lastInstances.resize(capacity);
    m_lastTransforms.resize(capacity);
    return m_tlasAddress != 0u;
}

// --- commit ---------------------------------------------------------------------------------------

bool AccelerationStructures::readCompactions(RtCommitStats& stats) {
    Impl& im = *m_impl;
    if (im.queries == VK_NULL_HANDLE) {
        return true;
    }
    bool ok = true;
    const u32 count = static_cast<u32>(m_blas.size());
    for (u32 mesh = 0; mesh < count; ++mesh) {
        Blas& b = m_blas[mesh];
        if (!b.info.compactQueued || b.info.buildSerial > m_completedSerial) {
            continue;
        }
        u64 compacted = 0;
        const VkResult r = vkGetQueryPoolResults(im.device, im.queries, mesh, 1, sizeof(u64), &compacted, sizeof(u64),
                                                 VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS) {
            continue; // not available yet
        }
        b.info.compactQueued = false;
        b.info.compactedSize = compacted;
        if (compacted == 0u || compacted > b.info.size || (compacted == b.info.size && !m_desc.forceCompactionCopy)) {
            continue; // nothing to gain
        }
        Buffer fresh{};
        if (!createAsBuffer(fresh, compacted, "rt.blas.compacted")) {
            ok = false;
            continue;
        }
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = static_cast<VkBuffer>(fresh.handle);
        ci.size = compacted;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        if (im.create(im.device, &ci, nullptr, &handle) != VK_SUCCESS) {
            m_desc.allocator->destroyBuffer(fresh);
            ok = false;
            continue;
        }
        BlasOp op{};
        op.mesh = mesh;
        op.kind = OpKind::Compact;
        op.srcHandle = b.handle;
        op.srcBuffer = b.buffer.handle;
        op.srcSize = static_cast<u64>(b.buffer.desc.size);
        op.dstHandle = handle;
        m_ops.push_back(op);
        ++m_compactOps;
        retire(b.handle, b.buffer); // destroyed once this frame's copy has completed
        b.handle = handle;
        b.buffer = fresh;
        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = handle;
        b.info.address = im.address(im.device, &ai);
        b.info.size = compacted;
        b.info.state = BlasState::Compacted;
        m_addressMirror[mesh] = b.info.address;
        m_addressDirty = true;
        ++stats.blasCompactions;
    }
    return ok;
}

bool AccelerationStructures::uploadAddressTable() {
    const u32 count = static_cast<u32>(m_addressMirror.size());
    if (count == 0u) {
        return true;
    }
    if (count > m_addressCapacity) {
        u32 capacity = std::max<u32>(m_addressCapacity, std::max<u32>(m_desc.meshCapacity, 16u));
        while (capacity < count) {
            capacity *= 2u;
        }
        if (!ensureBuffer(m_addressBuffer, static_cast<u64>(capacity) * sizeof(u64), kUsageTable, "rt.blas_addresses")) {
            return false;
        }
        m_addressCapacity = capacity;
    }
    const u8* bytes = reinterpret_cast<const u8*>(m_addressMirror.data());
    const usize total = static_cast<usize>(count) * sizeof(u64);
    const usize maxPiece = std::max<usize>((m_desc.upload->ringCapacity() / 2u) & ~usize{7u}, 8u);
    for (usize done = 0; done < total;) {
        const usize piece = std::min(maxPiece, total - done);
        usize ring = 0;
        if (!m_desc.upload->stage(bytes + done, piece, ring) ||
            !m_desc.upload->recordBufferCopy(m_addressBuffer.handle, ring, done, piece)) {
            return false;
        }
        done += piece;
    }
    m_addressDirty = false;
    return true;
}

TlasBuildMode AccelerationStructures::decideTlas(bool blasChanged) {
    const gpu_scene::GpuScene& scene = *m_desc.scene;
    const u32 n = scene.instanceHighWater();
    bool structural = !m_tlasBuilt || n != m_tlasCount || blasChanged;
    bool motion = false;
    for (u32 i = 0; i < n && !structural; ++i) {
        const GpuInstance& now = scene.instance(i);
        const GpuInstance& was = m_lastInstances[i];
        if (now.mesh != was.mesh || now.flags != was.flags) {
            structural = true;
            break;
        }
        if (!motion && std::memcmp(&scene.transform(i), &m_lastTransforms[i], sizeof(GpuTransform)) != 0) {
            motion = true;
        }
    }
    if (structural) {
        return TlasBuildMode::Build;
    }
    if (!motion && !m_blasRefitThisFrame) {
        return TlasBuildMode::None;
    }
    return m_tlasUpdates >= m_desc.maxTlasUpdates ? TlasBuildMode::Build : TlasBuildMode::Update;
}

bool AccelerationStructures::packInstancesCpu() {
    const gpu_scene::GpuScene& scene = *m_desc.scene;
    const u32 n = m_tlasCount;
    const u32 blasCount = static_cast<u32>(m_addressMirror.size());
    for (u32 i = 0; i < n; ++i) {
        m_cpuInstances[i] =
            packRtInstance(scene.instance(i), scene.transform(i), i, m_addressMirror.data(), blasCount, m_inactiveBlas, m_inactiveMask);
    }
    const u8* bytes = reinterpret_cast<const u8*>(m_cpuInstances.data());
    const usize total = static_cast<usize>(n) * sizeof(AsInstance);
    const usize maxPiece = std::max<usize>((m_desc.upload->ringCapacity() / 2u) & ~usize{63u}, 64u);
    for (usize done = 0; done < total;) {
        const usize piece = std::min(maxPiece, total - done);
        usize ring = 0;
        if (!m_desc.upload->stage(bytes + done, piece, ring) ||
            !m_desc.upload->recordBufferCopy(m_instanceBuffer.handle, ring, done, piece)) {
            return false;
        }
        done += piece;
    }
    return true;
}

RtCommitStats AccelerationStructures::commit() {
    RtCommitStats stats{};
    m_ops.clear();
    m_compactOps = 0;
    m_decodeOps = 0;
    m_queryOps = 0;
    m_scratchUsed = 0;
    m_blasRefitThisFrame = false;
    m_tlasMode = TlasBuildMode::None;
    if (!m_ready) {
        stats.ok = false;
        m_stats = stats;
        return stats;
    }
    Impl& im = *m_impl;
    const gpu_scene::GpuScene& scene = *m_desc.scene;
    const u32 meshCount = scene.meshCount();
    if (m_blas.size() < meshCount) {
        m_blas.resize(meshCount);
    }
    if (m_options.size() < meshCount) {
        m_options.resize(meshCount);
    }
    if (m_addressMirror.size() < meshCount) {
        m_addressMirror.resize(meshCount, 0u);
        m_addressDirty = true;
    }
    if (m_blasRefs.size() < m_blas.size()) {
        m_blasRefs.resize(m_blas.size());
    }

    // 1. Compactions whose sizes are readable (their copies run first this frame).
    stats.ok = readCompactions(stats) && stats.ok;

    // 2. New BLASes (decode + build) and refits, in two groups: decode builds first.
    u64 arenaBytes = 0;
    for (u32 pass = 0; pass < 2u; ++pass) {
        for (u32 mesh = 0; mesh < meshCount; ++mesh) {
            Blas& b = m_blas[mesh];
            const bool fresh = b.info.state == BlasState::None;
            if (pass == 0u && fresh) {
                const GpuMesh& gm = scene.mesh(mesh);
                if (gm.indexCount == 0u || gm.vertexCount == 0u || (gm.positions == 0u && !b.deformPending)) {
                    b.info.state = BlasState::Unsupported;
                    continue;
                }
                if (!createBlasObject(mesh, b)) {
                    b.info.state = BlasState::Failed;
                    stats.ok = false;
                    continue;
                }
                m_addressMirror[mesh] = b.info.address;
                m_addressDirty = true;
                b.info.state = BlasState::Pending;
                if (b.deformPending) {
                    continue; // built from the deformation source in the second group
                }
                BlasOp op{};
                op.mesh = mesh;
                op.kind = OpKind::Build;
                op.vertexAddress = arenaBytes; // arena offset, made absolute below
                op.query = (b.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR) != 0u;
                arenaBytes = alignUp(arenaBytes + static_cast<u64>(gm.vertexCount) * kVertexStride, 16u);
                m_ops.push_back(op);
                ++m_decodeOps;
                m_queryOps += op.query ? 1u : 0u;
                ++stats.blasBuilds;
                stats.decodedVertices += gm.vertexCount;
            } else if (pass == 1u && b.deformPending && b.handle != nullptr) {
                BlasOp op{};
                op.mesh = mesh;
                op.vertexAddress = b.deformAddress;
                op.srcBuffer = b.deformBuffer;
                op.srcSize = b.deformSize;
                if (b.info.state == BlasState::Pending) {
                    op.kind = OpKind::Build;
                    ++stats.blasBuilds;
                } else if ((b.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) != 0u &&
                           (b.info.state == BlasState::Built || b.info.state == BlasState::Compacted)) {
                    op.kind = OpKind::Update;
                    ++b.info.updates;
                    ++stats.blasUpdates;
                    m_blasRefitThisFrame = true;
                } else {
                    b.deformPending = false;
                    continue;
                }
                b.deformPending = false;
                m_ops.push_back(op);
            }
        }
    }

    // Scratch layout: one region per BLAS op, then the TLAS region (address-aligned).
    const u64 align = m_scratchAlignment;
    for (usize i = m_compactOps; i < m_ops.size(); ++i) {
        BlasOp& op = m_ops[i];
        const Blas& b = m_blas[op.mesh];
        op.scratchOffset = alignUp(m_scratchUsed, align);
        op.scratchSize = std::max<u64>(op.kind == OpKind::Update ? b.updateScratch : b.buildScratch, 1u);
        m_scratchUsed = op.scratchOffset + op.scratchSize;
    }

    // 3. Address table (new BLASes / compactions) before the TLAS decision reads m_addressDirty.
    const bool blasChanged = m_addressDirty;
    m_inactiveBlas = 0;
    m_inactiveMask = 0;
    const bool placeholder = m_desc.inactive == RtInactivePolicy::Placeholder ||
                             (m_desc.inactive == RtInactivePolicy::Auto && m_caps.inactiveInstanceQuirk);
    for (usize mesh = 0; placeholder && mesh < m_addressMirror.size(); ++mesh) {
        if (m_addressMirror[mesh] != 0u) {
            m_inactiveBlas = m_addressMirror[mesh];
            m_inactiveMask = kRtMaskDead;
            break;
        }
    }
    if (m_inactiveBlas != m_lastInactiveBlas) {
        m_lastInactiveBlas = m_inactiveBlas;
        m_addressDirty = true; // dead slots change reference: rebuild the TLAS
    }
    if (m_addressDirty && !uploadAddressTable()) {
        stats.ok = false;
    }

    // 4. TLAS.
    const u32 n = scene.instanceHighWater();
    if (!ensureTlas(n)) {
        stats.ok = false;
        m_stats = stats;
        return stats;
    }
    m_tlasMode = decideTlas(blasChanged);
    if (m_tlasMode != TlasBuildMode::None) {
        m_tlasCount = n;
        for (u32 i = 0; i < n; ++i) {
            m_lastInstances[i] = scene.instance(i);
            m_lastTransforms[i] = scene.transform(i);
        }
        m_tlasUpdates = m_tlasMode == TlasBuildMode::Update ? m_tlasUpdates + 1u : 0u;
        m_tlasScratchOffset = alignUp(m_scratchUsed, align);
        m_scratchUsed = m_tlasScratchOffset +
                        std::max<u64>(m_tlasMode == TlasBuildMode::Update ? m_tlasUpdateScratch : m_tlasBuildScratch, 1u);
        if (!m_gpuPacking) {
            stats.cpuPacked = true;
            if (!packInstancesCpu()) {
                stats.ok = false;
            }
        } else {
            m_instancesPush.instances = scene.header().addresses[static_cast<u32>(GpuSceneTable::Instances)];
            m_instancesPush.transforms = scene.header().addresses[static_cast<u32>(GpuSceneTable::Transforms)];
            m_instancesPush.blasTable = m_addressBuffer.deviceAddress;
            m_instancesPush.out = m_instanceBuffer.deviceAddress;
            m_instancesPush.count = n;
            m_instancesPush.blasCount = std::min<u32>(static_cast<u32>(m_addressMirror.size()), m_addressCapacity);
            m_instancesPush.inactiveBlas = m_inactiveBlas;
            m_instancesPush.inactiveMask = m_inactiveMask;
        }
        m_tlasBuilt = true;
    }

    // 5. Transient-ish buffers (grow only).
    if (arenaBytes > 0u && !ensureBuffer(m_decodeArena, arenaBytes, kUsageArena, "rt.decode_arena")) {
        stats.ok = false;
    }
    if (m_scratchUsed > 0u && !ensureBuffer(m_scratch, m_scratchUsed + align, kUsageScratch, "rt.scratch")) {
        stats.ok = false;
    }
    // Absolute decode addresses and the per-op push constants.
    im.decodePush.clear();
    for (u32 i = 0; i < m_decodeOps; ++i) {
        BlasOp& op = m_ops[m_compactOps + i];
        const GpuMesh& gm = scene.mesh(op.mesh);
        op.vertexAddress += m_decodeArena.deviceAddress;
        RtDecodePush push{};
        push.src = gm.positions;
        push.dst = op.vertexAddress;
        push.vertexCount = gm.vertexCount;
        for (u32 c = 0; c < 3u; ++c) {
            push.quantOffset[c] = gm.quantOffset[c];
            push.quantStep[c] = gm.quantStep[c];
        }
        im.decodePush.push_back(push);
    }
    for (usize i = m_compactOps; i < m_ops.size(); ++i) {
        Blas& b = m_blas[m_ops[i].mesh];
        b.info.buildSerial = m_frameSerial;
        if (m_ops[i].kind == OpKind::Build) {
            b.info.state = BlasState::Built;
            b.info.compactQueued = m_ops[i].query;
        }
    }
    const usize buildOps = m_ops.size() - m_compactOps;
    if (im.infos.size() < buildOps + 1u) {
        im.geometries.resize(buildOps + 1u);
        im.infos.resize(buildOps + 1u);
        im.ranges.resize(buildOps + 1u);
        im.rangePointers.resize(buildOps + 1u);
    }
    stats.tlas = m_tlasMode;
    stats.instances = m_tlasMode != TlasBuildMode::None ? m_tlasCount : 0u;
    stats.compactionQueries = m_queryOps;
    m_stats = stats;
    return stats;
}

// --- graph ----------------------------------------------------------------------------------------

RtGraphRefs AccelerationStructures::importInto(rg::Graph& graph, const gpu_scene::GpuSceneGraphRefs& sceneRefs) {
    RtGraphRefs refs{};
    if (!m_ready) {
        return refs;
    }
    refs.tlas = importRef(graph, m_tlasBuffer, "rt.tlas");
    refs.instances = importRef(graph, m_instanceBuffer, "rt.instances");
    const bool tlasPass = m_tlasMode != TlasBuildMode::None;
    if (m_ops.empty() && !tlasPass) {
        return refs;
    }
    if (m_blasRefs.size() < m_blas.size()) {
        m_blasRefs.resize(m_blas.size());
    }
    for (usize mesh = 0; mesh < m_blas.size(); ++mesh) {
        m_blasRefs[mesh] = importRef(graph, m_blas[mesh].buffer, "rt.blas");
    }
    rg::BufferRef scratch = importRef(graph, m_scratch, "rt.scratch");
    const u64 scratchBase = alignUp(m_scratch.deviceAddress, m_scratchAlignment) - m_scratch.deviceAddress;

    if (m_compactOps > 0u) {
        rg::PassBuilder pass = graph.addPass("rt.blas.compact", &AccelerationStructures::recordCompact, this);
        for (u32 i = 0; i < m_compactOps; ++i) {
            const BlasOp& op = m_ops[i];
            const rg::BufferRef src = graph.importBuffer(
                rg::ImportedBuffer{op.srcBuffer, op.srcSize, static_cast<u8>(rg::QueueClass::Graphics), nullptr, "rt.blas.old"});
            pass.use(src, rg::Access::AccelerationStructureBuildRead);
            pass.use(m_blasRefs[op.mesh], rg::Access::AccelerationStructureBuildWrite);
        }
    }
    const usize buildOps = m_ops.size() - m_compactOps;
    // One import per buffer and frame: the graph tracks hazards per imported resource.
    const rg::BufferRef arena = m_decodeOps > 0u ? importRef(graph, m_decodeArena, "rt.decode_arena") : rg::BufferRef{};
    if (m_decodeOps > 0u) {
        graph.addPass("rt.blas.decode", &AccelerationStructures::recordDecode, this)
            .use(arena, rg::Access::StorageWrite, {}, rg::kStageCompute);
    }
    if (buildOps > 0u) {
        rg::PassBuilder pass = graph.addPass("rt.blas.build", &AccelerationStructures::recordBlasBuild, this);
        if (m_decodeOps > 0u) {
            pass.use(arena, rg::Access::AccelerationStructureBuildInput);
        }
        if (sceneRefs.indices.valid()) {
            pass.use(sceneRefs.indices, rg::Access::AccelerationStructureBuildInput);
        }
        u64 blasScratchEnd = 0;
        for (usize i = m_compactOps; i < m_ops.size(); ++i) {
            const BlasOp& op = m_ops[i];
            if (op.srcBuffer != nullptr) {
                const rg::BufferRef input = graph.importBuffer(rg::ImportedBuffer{
                    op.srcBuffer, op.srcSize, static_cast<u8>(rg::QueueClass::Graphics), nullptr, "rt.deform_positions"});
                pass.use(input, rg::Access::AccelerationStructureBuildInput);
            }
            pass.use(m_blasRefs[op.mesh], rg::Access::AccelerationStructureBuildWrite);
            blasScratchEnd = op.scratchOffset + op.scratchSize;
        }
        pass.use(scratch, rg::Access::AccelerationStructureBuildWrite, rg::BufferRange{scratchBase, blasScratchEnd});
        if (m_queryOps > 0u) {
            rg::PassBuilder query = graph.addPass("rt.blas.compact_query", &AccelerationStructures::recordQuery, this);
            for (usize i = m_compactOps; i < m_ops.size(); ++i) {
                if (m_ops[i].query) {
                    query.use(m_blasRefs[m_ops[i].mesh], rg::Access::AccelerationStructureBuildRead);
                }
            }
            query.neverCull();
        }
    }
    if (tlasPass) {
        if (m_gpuPacking) {
            const rg::BufferRef table = importRef(graph, m_addressBuffer, "rt.blas_addresses");
            rg::PassBuilder pass = graph.addPass("rt.tlas.instances", &AccelerationStructures::recordInstances, this);
            const rg::BufferRef instances = sceneRefs.tables[static_cast<u32>(GpuSceneTable::Instances)];
            const rg::BufferRef transforms = sceneRefs.tables[static_cast<u32>(GpuSceneTable::Transforms)];
            if (instances.valid()) {
                pass.use(instances, rg::Access::StorageRead, {}, rg::kStageCompute);
            }
            if (transforms.valid()) {
                pass.use(transforms, rg::Access::StorageRead, {}, rg::kStageCompute);
            }
            if (table.valid()) {
                pass.use(table, rg::Access::StorageRead, {}, rg::kStageCompute);
            }
            pass.use(refs.instances, rg::Access::StorageWrite, rg::BufferRange{0, static_cast<u64>(m_tlasCount) * sizeof(AsInstance)},
                     rg::kStageCompute);
        }
        rg::PassBuilder pass = graph.addPass("rt.tlas.build", &AccelerationStructures::recordTlas, this);
        pass.use(refs.instances, rg::Access::AccelerationStructureBuildInput);
        for (usize mesh = 0; mesh < m_blas.size(); ++mesh) {
            if (m_blasRefs[mesh].valid()) {
                pass.use(m_blasRefs[mesh], rg::Access::AccelerationStructureBuildRead);
            }
        }
        pass.use(refs.tlas, rg::Access::AccelerationStructureBuildWrite);
        pass.use(scratch, rg::Access::AccelerationStructureBuildWrite,
                 rg::BufferRange{scratchBase + m_tlasScratchOffset,
                                 std::max<u64>(m_tlasMode == TlasBuildMode::Update ? m_tlasUpdateScratch : m_tlasBuildScratch, 1u)});
    }
    return refs;
}

void AccelerationStructures::recordCompact(const rg::PassContext& context, void* user) {
    AccelerationStructures& self = *static_cast<AccelerationStructures*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    for (u32 i = 0; i < self.m_compactOps; ++i) {
        const BlasOp& op = self.m_ops[i];
        VkCopyAccelerationStructureInfoKHR copy{};
        copy.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
        copy.src = static_cast<VkAccelerationStructureKHR>(op.srcHandle);
        copy.dst = static_cast<VkAccelerationStructureKHR>(op.dstHandle);
        copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        self.m_impl->cmdCopy(cmd, &copy);
    }
}

void AccelerationStructures::recordDecode(const rg::PassContext& context, void* user) {
    AccelerationStructures& self = *static_cast<AccelerationStructures*>(user);
    const Impl& im = *self.m_impl;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.decode);
    for (const RtDecodePush& push : im.decodePush) {
        vkCmdPushConstants(cmd, im.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RtDecodePush), &push);
        vkCmdDispatch(cmd, (push.vertexCount + kRtWorkgroupSize - 1u) / kRtWorkgroupSize, 1, 1);
    }
}

void AccelerationStructures::recordBlasBuild(const rg::PassContext& context, void* user) {
    AccelerationStructures& self = *static_cast<AccelerationStructures*>(user);
    Impl& im = *self.m_impl;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    const gpu_scene::GpuScene& scene = *self.m_desc.scene;
    const u64 scratchBase = alignUp(self.m_scratch.deviceAddress, self.m_scratchAlignment);
    u32 count = 0;
    for (usize i = self.m_compactOps; i < self.m_ops.size(); ++i, ++count) {
        const BlasOp& op = self.m_ops[i];
        const Blas& b = self.m_blas[op.mesh];
        const GpuMesh& gm = scene.mesh(op.mesh);
        VkAccelerationStructureGeometryKHR& g = im.geometries[count];
        g = VkAccelerationStructureGeometryKHR{};
        g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = op.vertexAddress;
        g.geometry.triangles.vertexStride = kVertexStride;
        g.geometry.triangles.maxVertex = gm.vertexCount - 1u;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress = scene.indexBuffer().deviceAddress;
        VkAccelerationStructureBuildGeometryInfoKHR& info = im.infos[count];
        info = VkAccelerationStructureBuildGeometryInfoKHR{};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = b.flags;
        info.mode = op.kind == OpKind::Update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                              : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.srcAccelerationStructure =
            op.kind == OpKind::Update ? static_cast<VkAccelerationStructureKHR>(b.handle) : VK_NULL_HANDLE;
        info.dstAccelerationStructure = static_cast<VkAccelerationStructureKHR>(b.handle);
        info.geometryCount = 1;
        info.pGeometries = &g;
        info.scratchData.deviceAddress = scratchBase + op.scratchOffset;
        VkAccelerationStructureBuildRangeInfoKHR& range = im.ranges[count];
        range = VkAccelerationStructureBuildRangeInfoKHR{};
        range.primitiveCount = gm.indexCount / 3u;
        range.primitiveOffset = gm.firstIndex * static_cast<u32>(sizeof(u32));
        im.rangePointers[count] = &range;
    }
    if (count > 0u) {
        im.cmdBuild(cmd, count, im.infos.data(), im.rangePointers.data());
    }
}

void AccelerationStructures::recordQuery(const rg::PassContext& context, void* user) {
    AccelerationStructures& self = *static_cast<AccelerationStructures*>(user);
    const Impl& im = *self.m_impl;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    for (usize i = self.m_compactOps; i < self.m_ops.size(); ++i) {
        const BlasOp& op = self.m_ops[i];
        if (!op.query) {
            continue;
        }
        VkAccelerationStructureKHR handle = static_cast<VkAccelerationStructureKHR>(self.m_blas[op.mesh].handle);
        vkCmdResetQueryPool(cmd, im.queries, op.mesh, 1);
        im.cmdWriteProperties(cmd, 1, &handle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, im.queries, op.mesh);
    }
}

void AccelerationStructures::recordInstances(const rg::PassContext& context, void* user) {
    AccelerationStructures& self = *static_cast<AccelerationStructures*>(user);
    const Impl& im = *self.m_impl;
    if (self.m_instancesPush.count == 0u) {
        return;
    }
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.instances);
    vkCmdPushConstants(cmd, im.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RtInstancesPush), &self.m_instancesPush);
    vkCmdDispatch(cmd, (self.m_instancesPush.count + kRtWorkgroupSize - 1u) / kRtWorkgroupSize, 1, 1);
}

void AccelerationStructures::recordTlas(const rg::PassContext& context, void* user) {
    AccelerationStructures& self = *static_cast<AccelerationStructures*>(user);
    Impl& im = *self.m_impl;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    const bool update = self.m_tlasMode == TlasBuildMode::Update;
    VkAccelerationStructureGeometryKHR& g = im.geometries.back();
    g = VkAccelerationStructureGeometryKHR{};
    g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.arrayOfPointers = VK_FALSE;
    g.geometry.instances.data.deviceAddress = self.m_instanceBuffer.deviceAddress;
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.srcAccelerationStructure = update ? static_cast<VkAccelerationStructureKHR>(self.m_tlas) : VK_NULL_HANDLE;
    info.dstAccelerationStructure = static_cast<VkAccelerationStructureKHR>(self.m_tlas);
    info.geometryCount = 1;
    info.pGeometries = &g;
    info.scratchData.deviceAddress = alignUp(self.m_scratch.deviceAddress, self.m_scratchAlignment) + self.m_tlasScratchOffset;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = self.m_tlasCount;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    im.cmdBuild(cmd, 1, &info, &ranges);
}

#else // !FUSE_RT_VULKAN: the T2 gate always fails; the API stays callable.

bool AccelerationStructures::init(const AccelerationStructuresDesc& desc) {
    m_desc = desc;
    m_caps = queryRtCapabilities(desc.device);
    m_reason = m_caps.usable ? "built without Vulkan acceleration-structure support" : m_caps.reason;
    m_ready = false;
    return false;
}

void AccelerationStructures::destroy() {
    m_ready = false;
    m_impl.reset();
}

void AccelerationStructures::destroyAs(void*, Buffer& buffer) {
    buffer = Buffer{};
}

RtCommitStats AccelerationStructures::commit() {
    RtCommitStats stats{};
    stats.ok = false;
    m_stats = stats;
    return stats;
}

RtGraphRefs AccelerationStructures::importInto(rg::Graph&, const gpu_scene::GpuSceneGraphRefs&) {
    return RtGraphRefs{};
}

#endif

} // namespace fuse::renderer::rt
