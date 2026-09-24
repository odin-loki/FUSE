#pragma once

// WP-3.1 virtual shadow maps, core (renderer plan Phase 3: "a 16k x 16k virtual shadow map per
// directional light with clipmap levels; page marking from visible pixels, a page table and a
// physical page pool; caching of static pages with invalidation when objects move").
//
// Virtual space: `levels` (<= 16) clipmap levels of 128 x 128 pages of 128 x 128 texels (16k^2 per
// level); level L's window is firstLevelExtent * 2^L light-space units wide, centred on the camera's
// page and addressed toroidally (vsm_clipmap.hpp, core_logic/vsm_pages). Physical space: an R32_UINT
// pool of poolPagesX x poolPagesY pages (<= 64 x 64). Everything is a render-graph v2 compute pass
// (no manual barriers). Per frame:
//
//   vsm.beginFrame(serial, {view, depth handle, scene handle, instance count});
//   VsmGraphRefs r = vsm.importInto(graph);
//   vsm.addFrame(graph, r, visRefs.depth, sceneRefs);   // or the pieces below, in this order:
//     addMarking      "vsm.reset" (counters, request / need bits; first frame also "vsm.init": page
//                     table 0, physical owners none), "vsm.mark" (one thread per depth texel ->
//                     request bits, vsm_kernel.hpp mark_pixel)
//     addInvalidation ["vsm.bounds_clear"], "vsm.invalidate" (GPU-scene bounds changes clear cached
//                     bits; see vsm_kernel.hpp)
//     addAllocation   "vsm.update" (window scroll / level invalidation, touch, need bits),
//                     "vsm.alloc" (one workgroup: deterministic age-bucketed LRU allocation),
//                     "vsm.render" (render list + cached bits)
//     addClearPages   "vsm.clear" (optional: dispatch indirect over the render list, clears the pages
//                     to render in the pool)
//
// The CPU model of the whole frame is core_logic/vsm_pages (VsmPagePool::update) fed with
// markReference() and VsmInvalidationReference (vsm_clipmap.hpp): same page table, same physical
// metadata, same statistics, same render-list set (fuse_rp_vsm_vk_* gates).
//
// WP-3.2 hooks (page rendering, filtering, local lights):
//   * the render list: work buffer words [offRenderList + 2i] = virtual page, [+1] = physical page,
//     i < counters[0]; counters[0..2] is a VkDispatchIndirectCommand (one workgroup per page) at byte
//     offset renderArgsOffset() of workBuffer() (BufferUsage::Indirect);
//   * the page table (u32 PTE per virtual page, mapped / cached / physical bits) and the pool
//     (bindless storage image, R32_UINT: depth as float bits, cleared to clearValue by vsm.clear, for
//     atomicMin rendering); frameConstantsAddress() = this frame's VsmFrameConstants (BDA) with the
//     level placement (origins, page size, depth key / centre / step) to build per-page projections;
//   * vsm_common.{glsl,slang} (include/fuse/renderer/shadow/vsm) hold the layouts and the slot math.
//   * A page stays cached (its pool texels valid) until an object moving over it, a window scroll,
//     a depth-key step, a light rotation or an eviction drops the cached bit: WP-3.2 renders exactly
//     the render list, so a static frame renders 0 pages.
//
// Steady-state frames make no heap allocations (fixed pass records; buffers change only when the
// instance count outgrows the bounds buffer). Stub backend / no device: init() fails; the CPU
// references (vsm_clipmap.hpp, vsm_kernel.hpp, core_logic) remain available.

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/shadow/vsm/vsm_clipmap.hpp>
#include <fuse/renderer/shadow/vsm/vsm_types.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::vsm {

enum class VsmKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct VsmCapabilities {
    bool vsm = false;
    const char* reason = "no device"; ///< "ok" when usable
};

/// What the device (as created) supports for the VSM passes (BDA, R32_UINT storage images).
VsmCapabilities queryVsmCapabilities(const VulkanDevice* device);

/// Byte layout of the work buffer (every section 256-aligned; offsets in bytes).
struct VsmWorkLayout {
    u64 counters = 0;    ///< kCounterCount u32 (the render list's dispatch args first)
    u64 request = 0;     ///< requestWords u32
    u64 need = 0;        ///< requestWords u32
    u64 renderList = 0;  ///< physPages x {virtual, physical}
    u64 candidates = 0;  ///< physPages u32
    u64 resetBytes = 0;  ///< [request, need end): zeroed by vsm.reset
    u64 bytes = 0;

    static VsmWorkLayout compute(u32 levels, u32 physPages);
};

struct VirtualShadowMapDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    VsmClipmapDesc clipmap{};       ///< levels, level-0 extent, marking radius, LOD bias
    u32 poolPagesX = 16;            ///< physical pool, pages per axis (<= 64); 16 x 16 = a 2k^2 atlas
    u32 poolPagesY = 16;
    u32 instanceCapacity = 1024;    ///< initial bounds-tracking capacity (grows by doubling)
    u32 framesInFlight = 3;         ///< frame-constant ring slots
    u32 clearValue = 0x3F800000u;   ///< vsm.clear value: float bits of 1.0 (far, forward depth)
    VsmKernelLanguage language = VsmKernelLanguage::Auto;
    const char* name = "vsm";
};

struct VsmFrameDesc {
    VsmViewDesc view{};             ///< light direction, camera, the depth's inverse viewProj and extent
    u32 depthHandle = 0;            ///< bindless sampled handle of the depth (VisBuffer::depthSampledHandle())
    u32 scene = 0;                  ///< GpuScene::headerHandle()
    u32 instanceCount = 0;          ///< GpuScene::instanceHighWater()
};

/// Render-graph handles of the VSM resources for one frame (importInto()).
struct VsmGraphRefs {
    rg::BufferRef pageTable;
    rg::BufferRef physMeta;
    rg::BufferRef work;
    rg::BufferRef bounds;
    rg::TextureRef pool;
};

struct VsmStats {
    u32 boundsRebuilds = 0;  ///< bounds buffer (re)creations (the history restarts: VsmInvalidationReference::reset)
    u32 retired = 0;
    u32 passes = 0;          ///< vsm.* passes added this frame
};

class VirtualShadowMap {
public:
    VirtualShadowMap() = default;
    ~VirtualShadowMap();
    VirtualShadowMap(const VirtualShadowMap&) = delete;
    VirtualShadowMap& operator=(const VirtualShadowMap&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the
    /// requested language, with an invalid clipmap / pool description, or in the stub backend.
    bool init(const VirtualShadowMapDesc& desc);
    /// The caller must have retired every frame that used the resources.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Places the clipmap (VsmClipmap::build) and writes this frame's constants (ring slot of
    /// `frameSerial`); grows the bounds buffer when the instance count exceeds it. Every begun frame
    /// must run addAllocation() (the GPU page state follows the placement history).
    bool beginFrame(u64 frameSerial, const VsmFrameDesc& frame);
    VsmGraphRefs importInto(rg::Graph& graph);

    /// "vsm.reset" (+ "vsm.init" on the first frame) and "vsm.mark" from `depth` (declared SampledRead
    /// from compute; beginFrame's depthHandle must be a sampled view of it).
    void addMarking(rg::Graph& graph, const VsmGraphRefs& refs, rg::TextureRef depth);
    /// ["vsm.bounds_clear"] + "vsm.invalidate" over the GPU scene's instances.
    void addInvalidation(rg::Graph& graph, const VsmGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene);
    /// "vsm.update", "vsm.alloc", "vsm.render".
    void addAllocation(rg::Graph& graph, const VsmGraphRefs& refs);
    /// "vsm.clear": clears the physical pages of the render list (dispatch indirect).
    void addClearPages(rg::Graph& graph, const VsmGraphRefs& refs);
    /// addMarking + addInvalidation + addAllocation (+ addClearPages when `clearPages`).
    void addFrame(rg::Graph& graph, const VsmGraphRefs& refs, rg::TextureRef depth, const gpu_scene::GpuSceneGraphRefs& scene,
                  bool clearPages = true);

    /// Destroys buffers retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection / WP-3.2 hooks ---------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    /// This frame's constants (host copy of what the GPU reads).
    const VsmFrameConstants& constants() const { return m_constants; }
    u64 frameConstantsAddress() const { return m_frameAddress; }
    const VsmWorkLayout& workLayout() const { return m_layout; }
    const Buffer& pageTableBuffer() const { return m_pageTable.buffer; }
    const Buffer& physMetaBuffer() const { return m_physMeta.buffer; }
    const Buffer& workBuffer() const { return m_work.buffer; }
    const Buffer& boundsBuffer() const { return m_bounds.buffer; }
    const Texture& poolImage() const { return m_pool.image; }
    u32 poolStorageHandle() const { return m_poolHandle; }
    u32 pageTableHandle() const { return m_pageTable.handle; }
    u64 renderArgsOffset() const { return m_layout.counters; }
    u32 levels() const { return m_clipmap.desc().levels; }
    u32 physPages() const { return m_desc.poolPagesX * m_desc.poolPagesY; }
    u32 boundsCapacity() const { return m_boundsCapacity; }
    const VsmStats& stats() const { return m_stats; }

private:
    struct OwnedBuffer {
        Buffer buffer{};
        BindlessSlotHandle slot{};
        u32 handle = 0;
        u8 queue = rg::kNoQueue;
    };
    struct Pool {
        Texture image{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    struct Retired {
        OwnedBuffer buffer{};
        u64 serial = 0;
    };
    enum Kernel : u32 { kMark = 0, kInvalidate, kUpdate, kAlloc, kRender, kClear, kKernelCount };
    enum RecordKind : u8 { kDispatch = 0, kDispatchIndirect, kReset, kInit, kBoundsClear };
    struct PassRecord {
        VirtualShadowMap* self = nullptr;
        VsmPush push{};
        RecordKind kind = kDispatch;
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
    };
    static constexpr u32 kMaxPasses = 16u;

    bool createPipelines();
    bool createBuffer(OwnedBuffer& out, u64 bytes, bool indirect, const char* name);
    void destroyBuffer(OwnedBuffer& b);
    PassRecord* nextRecord(RecordKind kind, u32 kernel);
    static void recordPass(const rg::PassContext& context, void* user);

    VirtualShadowMapDesc m_desc{};
    VsmClipmap m_clipmap{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    VsmWorkLayout m_layout{};
    OwnedBuffer m_pageTable{};
    OwnedBuffer m_physMeta{};
    OwnedBuffer m_work{};
    OwnedBuffer m_bounds{};
    Buffer m_frameRing{};
    Pool m_pool{};
    BindlessSlotHandle m_poolSlot{};
    u32 m_poolHandle = 0;
    BindlessSlotHandle m_sampler{};
    u32 m_samplerHandle = 0;
    u32 m_boundsCapacity = 0;
    u32 m_boundsParity = 0;     ///< which half vsm.invalidate writes next
    bool m_needInit = true;     ///< page table / physical metadata not initialised yet
    bool m_boundsDirty = true;  ///< bounds buffer must be cleared before its next use
    u32 m_lastInstanceCount = 0;
    VsmFrameConstants m_constants{};
    u64 m_frameAddress = 0;
    std::vector<Retired> m_retired;
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push, compute)
    void* m_pipelines[kKernelCount] = {};
    VsmStats m_stats{};
};

} // namespace fuse::renderer::vsm
