#pragma once

// WP-9.2 3D Gaussian splatting on Vulkan (renderer plan P9 "3D Gaussian splatting renderer for scanned assets,
// composited with the visibility buffer"; execution doc WP-9.2). Kerbl et al. 2023 tile rasteriser as compute
// passes on render graph v2, sorted with the existing GPU radix sort (compute/gpu_radix_sort.hpp).
//
//   GsplatRenderer gs; gs.init({device, allocator, bindless, ..., maxSplats, maxEntries});
//   gs.setSplats(asset.splats.data(), count, asset.shDegree);       // a GsAsset from gs_load_ply
//   gs.beginFrame(serial, camera, settings, width, height, &visDepth); // WP-1.4 export depth (R32F) or D32
//   GsGraphRefs refs = gs.importInto(graph);
//   gs.addPasses(graph, refs, visDepthRef);                          // after the visibility buffer
//   // gs.outputAddress(): f32x4 per pixel, (premultiplied splat radiance, transmittance T); a consumer
//   // composites lit' = rgb + T x lit (declare a StorageRead of refs.image).
//
// Passes (all compute, declared accesses only, no manual barriers; per-frame constants in a host ring read
// through BDA; Slang primary (-fp-mode precise) + GLSL twins (`precise`), embedded by cmake/rp_wp92.cmake):
//   gsplat.preprocess  per splat: cull, 3D -> 2D covariance, conic, radius, tile rect, SH radiance
//   gsplat.scan        one workgroup: tile-count prefix sum, counters, zeroed ranges, sort padding
//   gsplat.emit        per splat: (tile << 32 | bits(view z), splat) per touched tile
//   gsplat.sort        GpuRadixSort::recordSort over the whole entry capacity (u64 keys, an even pass count)
//   gsplat.ranges      per sorted entry: each tile's [begin, end)
//   gsplat.raster      per 16 x 16 tile: front-to-back alpha blending, early termination, depth composite
// The CPU reference (gsplat_reference.hpp) is the oracle of every pass. Steady-state frames make no heap
// allocation (buffers follow maxSplats / maxEntries / the extent; the depth binding changes only with the image).

#include <fuse/renderer/compute/gpu_radix_sort.hpp>
#include <fuse/renderer/gsplat/gsplat_reference.hpp>
#include <fuse/renderer/gsplat/gsplat_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::gsplat {

enum class GsKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct GsCapabilities {
    bool gsplat = false;
    const char* reason = "no device"; ///< "ok" when usable
};

GsCapabilities queryGsplatCapabilities(const VulkanDevice* device);

struct GsplatDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    GsKernelLanguage language = GsKernelLanguage::Auto;
    u32 framesInFlight = 3;
    u32 maxSplats = 1u << 16;
    u32 maxEntries = 1u << 20; ///< sort capacity (tile x splat pairs); overflow is counted and dropped
};

struct GsGraphRefs {
    rg::BufferRef splats;
    rg::BufferRef work;   ///< projected, offsets, counters
    rg::BufferRef keys;
    rg::BufferRef values;
    rg::BufferRef keysTemp;
    rg::BufferRef valuesTemp;
    rg::BufferRef scratch;
    rg::BufferRef image;  ///< ranges, output
};

enum class GsCopySource : u8 {
    Projected = 0, ///< GsProjected[splatCount]
    Offsets,       ///< u32[splatCount]
    Counters,      ///< u32[4]
    Keys,          ///< u64[capacity]
    Values,        ///< u32[capacity]
    Ranges,        ///< u32x2[tileCount]
    Output,        ///< f32x4[width * height]
};

struct GsStats {
    u32 passes = 0;        ///< passes added this frame
    u32 imageRebuilds = 0;
    u32 depthBinds = 0;
    u32 retired = 0;
    u32 keyBits = 0;
    u32 sortPasses = 0;    ///< radix digits of this frame's sort
};

class GsplatRenderer {
public:
    GsplatRenderer() = default;
    ~GsplatRenderer();
    GsplatRenderer(const GsplatRenderer&) = delete;
    GsplatRenderer& operator=(const GsplatRenderer&) = delete;

    /// Fails (false, nothing created) without a capable device, a built kernel of the requested language or
    /// the radix sort shaders, or in the stub backend.
    bool init(const GsplatDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Copies the splats into the (host-visible) splat buffer. No frame using the buffer may be in flight.
    /// False when count > maxSplats.
    bool setSplats(const GsSplat* splats, u32 count, u32 shDegree);

    /// Resolves and writes this frame's constants (ring slot of `frameSerial`); follows the extent (ranges +
    /// output) and the depth image (bindless slot; null = no composite). False for an invalid camera / extent
    /// or a failed (re)allocation.
    bool beginFrame(u64 frameSerial, const GsCamera& camera, const GsSettings& settings, u32 width, u32 height,
                    const Texture* depth);

    GsGraphRefs importInto(rg::Graph& graph);
    /// gsplat.preprocess, .scan, .emit, .sort, .ranges, .raster. `depth`: the graph ref of beginFrame's depth
    /// image (ignored when none was given).
    void addPasses(rg::Graph& graph, const GsGraphRefs& refs, rg::TextureRef depth);
    /// Copies a section into `dst` at `dstOffset` (after addPasses).
    void addCopy(rg::Graph& graph, const GsGraphRefs& refs, GsCopySource what, rg::BufferRef dst, u64 dstOffset);
    u64 copyBytes(GsCopySource what) const;

    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const GsFrameConstants& constants() const { return m_constants; }
    const GsStats& stats() const { return m_stats; }
    u32 splatCount() const { return m_splatCount; }
    u32 capacity() const { return m_desc.maxEntries; }
    u64 outputAddress() const { return m_constants.output; }
    const Buffer& imageBuffer() const { return m_image; }

private:
    struct PassRecord {
        GsplatRenderer* self = nullptr;
        GsPush push{};
        u32 kernel = 0;
        u32 groups = 1;
        u64 copySrc = 0;
        u64 copyDst = 0;
        u64 copyBytes = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    struct Retired {
        Buffer buffer{};
        BindlessSlotHandle slot{};
        u64 serial = 0;
    };
    enum Kernel : u32 { kPreprocess = 0, kScan, kEmit, kRanges, kRaster, kKernelCount };
    static constexpr u32 kMaxPasses = 24u;

    bool createPipelines();
    bool createBuffer(Buffer& out, u64 bytes, bool hostVisible, bool transfer, const char* name);
    bool createImageBuffer(u32 width, u32 height, u32 tileCount);
    bool bindDepth(const Texture* depth);
    void retire(const Retired& r);
    void destroyRetired(Retired& r);
    PassRecord* nextRecord(u32 kernel, u32 groups);
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordSortPass(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    GsplatDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_splatCount = 0;
    u32 m_shDegree = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_tileCount = 0;

    Buffer m_splats{};
    Buffer m_work{};
    Buffer m_keys{};
    Buffer m_values{};
    Buffer m_keysTemp{};
    Buffer m_valuesTemp{};
    Buffer m_scratch{};
    Buffer m_image{};
    Buffer m_frameRing{};
    u8 m_queues[8] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue,
                      rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};
    u64 m_projectedOffset = 0;
    u64 m_offsetsOffset = 0;
    u64 m_countersOffset = 0;
    u64 m_outputOffset = 0;
    u64 m_frameAddress = 0;
    GsFrameConstants m_constants{};
    std::vector<Retired> m_retired;

    void* m_depthImage = nullptr;
    BindlessSlotHandle m_depthSlot{};
    u32 m_depthHandle = 0;

    std::unique_ptr<GpuRadixSort> m_sort;
    std::unique_ptr<GpuRadixSortBinding> m_sortBinding;
    u32 m_keyBits = 0;

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push)
    void* m_pipelines[kKernelCount] = {};

    GsStats m_stats{};
};

} // namespace fuse::renderer::gsplat
