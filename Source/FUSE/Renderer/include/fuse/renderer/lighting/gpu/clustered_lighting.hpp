#pragma once

// WP-2.1 GPU clustered lighting (renderer plan Phase 2: "froxel / cluster grid with a compute
// light-assignment pass; point, spot and directional lights"; execution doc WP-2.1).
//
// Input: the WP-1.1 GPU scene's light table (GpuLight rows, reached through the header handle and
// BDA) and the WP-1.5 G-buffer (GBufferAttachment RT0..RT5, write_gbuffer packing; RT4 holds the
// device depth, forward z/w since WP-1.3, so ClusterCameraDesc::reversedZ = false for it). Output:
// per-cluster light lists in GPU buffers, identical to the B5 oracle (ClusteredLightCuller /
// clustered_kernel.hpp), and an RGBA16F lit image.
//
//   lighting.beginFrame(serial, {camera, scene.headerHandle(), scene.lightHighWater(), ambient, &resolve});
//   LightingGraphRefs l = lighting.importInto(graph);
//   lighting.addAssignment(graph, l, sceneRefs);              // light.bounds / bin / cull / scan / compact
//   lighting.addShade(graph, l, sceneRefs, resolveRefs);      // light.shade, after the resolve
//
// Assignment (all compute, all declared on the render graph; no manual barriers):
//   light.bounds   one thread per cluster: view-space AABB from the host edge tables (bit-identical to
//                  clustered_kernel::build_cluster_aabb); one thread per light slot: view-space sphere
//                  + candidate slice window (point and spot lights, range sphere; the window is widened
//                  by kSliceWindowPad, see clustered_gpu_types.hpp).
//   light.bin      one 64-wide workgroup per depth slice: the slots whose window covers the slice, in
//                  ascending order (chunked workgroup scan, deterministic), and one more workgroup for the
//                  directional list.
//   light.cull     one thread per cluster: walks its slice's list in order, exact sphere / AABB test (the
//                  oracle's), keeps the first `capacity` hits, counts the rest as dropped.
//   light.scan     one 256-wide workgroup: exclusive scan of the counts -> grid (offset, count) + totals.
//   light.compact  one thread per cluster: copies its list to the flat list at its offset.
// Shade: light.shade, 8 x 8 tiles, one pixel per thread (clustered_gpu_kernel.hpp is the reference):
// G-buffer texelFetch (bindless sampled images), world position from depth, cluster lookup, the
// directional list then the cluster's point / spot lights, Cook-Torrance BRDF, + emissive + ambient.
// WP-2.2 (energyCompensation, the default): multi-scatter compensated BRDF, rectangle / disk area
// lights (LTC; clustered by their range sphere) and sun disks, from a LUT uploaded once at init.
//
// Kernels: Slang primary (-fp-mode precise) with GLSL twins (`precise`), embedded by
// cmake/rp_wp21.cmake. Steady-state frames make no heap allocations (buffers grow only when the light
// count exceeds the capacity; the G-buffer bindings change only when its images do).

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_types.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::lighting_gpu {

enum class LightingKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct LightingCapabilities {
    bool lighting = false;
    const char* reason = "no device"; ///< "ok" when usable
};

LightingCapabilities queryLightingCapabilities(const VulkanDevice* device);

/// Byte layout of the two buffers for a grid / capacity (every section 256-aligned).
struct LightingBufferLayout {
    // work buffer
    u64 aabbs = 0;
    u64 bounds = 0;
    u64 sliceCounts = 0;
    u64 sliceLights = 0;
    u64 clusterCounts = 0;
    u64 clusterDropped = 0;
    u64 clusterSlots = 0;
    u64 workBytes = 0;
    // lists buffer
    u64 header = 0;
    u64 grid = 0;
    u64 directional = 0;
    u64 lightList = 0;
    u64 listsBytes = 0;

    static LightingBufferLayout compute(u32 clusterCount, u32 slicesZ, u32 capacity, u32 lightCapacity);
};

struct ClusteredLightingDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    /// Grid (clamped with ClusterDesc::clampCounts). maxLightsPerCluster 0 (the oracle's "unlimited")
    /// becomes kMaxLightsPerCluster: lists are equal to the oracle's while no cluster exceeds it.
    ClusterDesc clusters{};
    u32 lightCapacity = 4096; ///< initial light-slot capacity (grows on demand)
    u32 width = 0;            ///< initial output extent (follows the G-buffer at beginFrame)
    u32 height = 0;
    LightingKernelLanguage language = LightingKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant ring slots
    const char* name = "clustered_lighting";
    /// WP-2.2: shade with the multi-scatter compensated BRDF, rectangle / disk area lights (LTC) and
    /// sun disks: init uploads ltc::sharedBrdfLut() once (LightingFrameConstants::brdfLut). false =
    /// the WP-2.1 single-scatter lobe (area lights contribute nothing).
    bool energyCompensation = true;
};

struct LightingFrameDesc {
    /// Must describe the projection the G-buffer depth was rendered with (position / forward / up /
    /// fovY / near / far, depth convention). The grid spans [nearPlane, farPlane].
    ClusterCameraDesc camera{};
    u32 scene = 0;       ///< GpuScene::headerHandle()
    u32 lightCount = 0;  ///< GpuScene::lightHighWater()
    f32 ambient[3] = {0.f, 0.f, 0.f};
    /// G-buffer source for addShade (its RT0, RT1, RT2, RT4 and RT5 are bound as bindless sampled
    /// images; rebound only when the images change). Null: assignment only.
    const material_resolve::MaterialResolve* gbuffer = nullptr;
};

struct LightingGraphRefs {
    rg::BufferRef work;
    rg::BufferRef lists;
    rg::TextureRef output; ///< RGBA16F lit image
};

struct LightingStats {
    u32 bufferRebuilds = 0;
    u32 outputRebuilds = 0;
    u32 gbufferBinds = 0;
    u32 retired = 0;
    u32 assignmentPasses = 0; ///< this frame
    u32 shadePasses = 0;      ///< this frame
};

class ClusteredLighting {
public:
    ClusteredLighting() = default;
    ~ClusteredLighting();
    ClusteredLighting(const ClusteredLighting&) = delete;
    ClusteredLighting& operator=(const ClusteredLighting&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the
    /// requested language, or in the stub backend.
    bool init(const ClusteredLightingDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Writes this frame's constants (ring slot of `frameSerial`); grows the buffers when
    /// `frame.lightCount` exceeds the capacity, follows the G-buffer's extent and images.
    bool beginFrame(u64 frameSerial, const LightingFrameDesc& frame);
    LightingGraphRefs importInto(rg::Graph& graph);

    /// light.bounds, light.bin, light.cull, light.scan, light.compact.
    void addAssignment(rg::Graph& graph, const LightingGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene);
    /// light.shade over the G-buffer of beginFrame's `gbuffer` (`gbufferRefs` = its importInto()).
    /// `dump`, when valid: also writes f32x4 radiance per pixel at `dumpAddress` (parity gates).
    void addShade(rg::Graph& graph, const LightingGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                  const material_resolve::ResolveGraphRefs& gbufferRefs, rg::BufferRef dump = {}, u64 dumpAddress = 0);

    /// Destroys buffers / images / bindless slots retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const ClusterDesc& clusterDesc() const { return m_clusters; }
    u32 capacity() const { return m_capacity; }
    u32 lightCapacity() const { return m_lightCapacity; }
    const LightingBufferLayout& layout() const { return m_layout; }
    const Buffer& workBuffer() const { return m_buffers.work; }
    const Buffer& listsBuffer() const { return m_buffers.lists; }
    const Texture& outputImage() const { return m_output.image; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const LightingStats& stats() const { return m_stats; }
    /// The LUT the shade reads (ltc::kLutWords f32, for ShadeReferenceDesc::brdfLut); null without
    /// energy compensation.
    const f32* brdfLut() const { return m_brdfLut; }
    /// BDA of this frame's LightingFrameConstants (valid after beginFrame, until the next one):
    /// consumers of the cluster lists (WP-2.3 forward transparency) read the grid / lists / camera
    /// through it, together with a StorageRead of LightingGraphRefs::lists.
    u64 frameConstantsAddress() const { return m_frameAddress; }

private:
    struct Buffers {
        Buffer work{};
        Buffer lists{};
        u8 queues[2] = {rg::kNoQueue, rg::kNoQueue};
    };
    struct Output {
        Texture image{};
        BindlessSlotHandle slot{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    static constexpr u32 kGBufferInputs = 5u; ///< RT0, RT1, RT2, RT4, RT5
    struct Retired {
        Buffer buffers[2]{};
        Texture image{};
        BindlessSlotHandle slots[1u + kGBufferInputs]{};
        u64 serial = 0;
    };
    struct PassRecord {
        ClusteredLighting* self = nullptr;
        LightingPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
    };
    enum Kernel : u32 { kBounds = 0, kBin, kCull, kScan, kCompact, kShade, kKernelCount };
    static constexpr u32 kMaxPasses = 16u;

    bool createPipelines();
    bool createBuffers(Buffers& b, u32 lightCapacity, LightingBufferLayout& layout);
    bool createOutput(Output& o, u32 width, u32 height);
    void retire(Retired r);
    void destroyRetired(Retired& r);
    bool bindGBuffer(const material_resolve::MaterialResolve& gbuffer);
    PassRecord* nextRecord();
    static void recordDispatch(const rg::PassContext& context, void* user);

    ClusteredLightingDesc m_desc{};
    ClusterDesc m_clusters{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_capacity = 0;
    u32 m_lightCapacity = 0;
    u32 m_lightCount = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    LightingBufferLayout m_layout{};
    Buffers m_buffers{};
    Output m_output{};
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    Buffer m_lutBuffer{};          ///< WP-2.2 BRDF / LTC LUT (host-visible, written once at init)
    const f32* m_brdfLut = nullptr;
    const material_resolve::MaterialResolve* m_gbuffer = nullptr;
    void* m_gbufferImages[kGBufferInputs] = {};
    BindlessSlotHandle m_gbufferSlots[kGBufferInputs]{};
    u32 m_gbufferHandles[kGBufferInputs] = {};

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push, compute)
    void* m_pipelines[kKernelCount] = {};

    LightingStats m_stats{};
};

} // namespace fuse::renderer::lighting_gpu
