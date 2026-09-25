#pragma once

// Asset plan W0.7: layered materials on Vulkan (compute passes on render graph v2). The evaluation and the CPU
// reference are in ml_kernel.hpp; the format is fusemat.hpp; the library (textures + resolved materials) is
// ml_reference.hpp's MlLibrary.
//
//   layers.setLibrary(library);                 // load time: uploads materials, texture pool, LUT, balls
//   layers.beginFrame(serial, frame);           // this frame's MlParams (camera, light, image size)
//   MaterialLayerRefs r = layers.importInto(graph);
//   layers.addEval(graph, r, src, srcAddr, dst, dstAddr, count);   // MlSurface[count] -> MlResult[count]
//   layers.addBalls(graph, r);                  // golden material-ball scene into the image section
//   layers.addCopyImage(graph, r, readback, 0);
//
// Passes (compute, every access declared on the graph, no manual barriers):
//   materials.eval    one thread per surface record (the layered evaluation a material resolve runs per pixel)
//   materials.balls   one thread per pixel of the golden scene
//   materials.copy    image -> caller buffer
// Kernels: Slang primary (-fp-mode precise) with GLSL twins, embedded by cmake/rp_w07.cmake. Steady-state frames
// make no heap allocation (the image buffer follows the size; pass records live in a fixed array).
//
// WP-1.5 integration (the material resolve's layered bin): setLibrary(library, &upload) also uploads every texture
// as a mip-mapped bindless sampled image (the ml_mips.hpp chain, through UploadQueue: flush + wait before the first
// resolve that reads them) and builds "materials.resolve_table" (MlResolveTable + the materials + MlTexture rows
// whose offset is the image's bindless handle), registered as a bindless storage buffer:
//
//   ResolveFrameDesc::layered = layers.resolveTableHandle();
//   MaterialLayerRefs r = layers.importInto(graph);
//   resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Binned, r.resolveTable);
//
// The GPU scene's material rows opt in with gpu_scene::kGpuMaterialLayered + the table index
// (gpu_scene::set_gpu_material_layered).

#include <fuse/renderer/material_layers/ml_reference.hpp>
#include <fuse/renderer/material_layers/ml_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::material_layers {

enum class MlKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct MlCapabilities {
    bool layers = false;
    const char* reason = "no device"; ///< "ok" when usable
};

MlCapabilities queryMaterialLayerCapabilities(const VulkanDevice* device);

struct MaterialLayersDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr; ///< the pipelines use its layout
    MlKernelLanguage language = MlKernelLanguage::Auto;
    u32 framesInFlight = 3;
};

/// Camera / light / image of a frame (the library addresses are filled in by beginFrame).
struct MlFrameDesc {
    MlParams params{};
};

struct MaterialLayerRefs {
    rg::BufferRef library;      ///< materials, textures, texel pool, LUT, balls (read only)
    rg::BufferRef image;        ///< the ball-scene image (f32x4 per pixel); invalid before the first beginFrame
    rg::BufferRef resolveTable; ///< the resolve's layered-material table (read only); invalid without one
};

struct MaterialLayerStats {
    u32 libraryUploads = 0;
    u32 imageRebuilds = 0;
    u32 retired = 0;
    u32 passes = 0; ///< this frame
    u32 resolveImages = 0;      ///< mip-mapped bindless images of the resolve table
    u64 resolveImageBytes = 0;  ///< their texel bytes (every level)
};

/// Byte layout of the library buffer (256-aligned sections).
struct MlLibraryLayout {
    u64 lut = 0;
    u64 textures = 0;
    u64 materials = 0;
    u64 balls = 0;
    u64 texels = 0;
    u64 bytes = 0;
    static MlLibraryLayout compute(u32 textureCount, u32 materialCount, u32 ballCount, u64 texelCount);
};

class MaterialLayers {
public:
    MaterialLayers() = default;
    ~MaterialLayers();
    MaterialLayers(const MaterialLayers&) = delete;
    MaterialLayers& operator=(const MaterialLayers&) = delete;

    /// Fails (false, nothing created) without a capable device or a built kernel of the requested language, or
    /// in the stub backend.
    bool init(const MaterialLayersDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Load time: uploads the library into a new buffer (the old one is retired). Allocates. With `upload`, also
    /// builds the WP-1.5 resolve table: every texture as a mip-mapped bindless image staged + recorded on `upload`
    /// (the caller flushes it; the images are readable by graphics work submitted after that flush) and the
    /// "materials.resolve_table" buffer (MlResolveTable header at offset 0) with its bindless handle.
    bool setLibrary(const MlLibrary& library, UploadQueue* upload = nullptr);
    /// Bindless storage-buffer handle of the resolve table (ResolveFrameDesc::layered); 0 without one.
    u32 resolveTableHandle() const { return m_resolveTableHandle; }
    /// Bindless sampled-image handle of texture `t` in the resolve table (0 without one).
    u32 resolveImageHandle(u32 t) const { return t < m_resolveImageHandles.size() ? m_resolveImageHandles[t] : 0u; }
    /// This frame's MlParams into its ring slot; (re)allocates the image when its size changes.
    bool beginFrame(u64 frameSerial, const MlFrameDesc& frame);

    MaterialLayerRefs importInto(rg::Graph& graph);
    /// MlSurface[count] at srcAddress (in srcBuffer) -> MlResult[count] at dstAddress (in dstBuffer).
    void addEval(rg::Graph& graph, const MaterialLayerRefs& refs, rg::BufferRef srcBuffer, u64 srcAddress,
                 rg::BufferRef dstBuffer, u64 dstAddress, u32 count);
    void addBalls(rg::Graph& graph, const MaterialLayerRefs& refs);
    void addCopyImage(rg::Graph& graph, const MaterialLayerRefs& refs, rg::BufferRef dst, u64 dstOffset);
    u64 imageBytes() const { return static_cast<u64>(m_params.width) * m_params.height * 16u; }

    u32 collectRetired(u64 completedSerial);

    const MlParams& params() const { return m_params; }
    u64 frameAddress() const { return m_frameAddress; }
    const MaterialLayerStats& stats() const { return m_stats; }
    const char* kernelLanguage() const { return m_language; }

private:
    struct Retired {
        Buffer buffer{};
        Texture image{};
        u64 serial = 0;
    };
    struct PassRecord {
        MaterialLayers* self = nullptr;
        MlPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
        u64 copyBytes = 0;
        u64 copyDst = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    enum Kernel : u32 { kEval = 0, kBalls, kKernelCount };
    static constexpr u32 kMaxPasses = 16u;

    bool createPipelines();
    bool createBuffer(u64 bytes, const char* name, bool hostVisible, Buffer& target, u8& queue);
    void retire(Buffer& buffer);
    void retireResolveTable();
    bool buildResolveTable(const MlLibrary& library, UploadQueue& upload);
    PassRecord* nextRecord(u32 kernel);
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    MaterialLayersDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    MlParams m_params{};
    MlLibraryLayout m_layout{};
    u32 m_textureCount = 0;
    u32 m_materialCount = 0;
    u32 m_ballCount = 0;
    Buffer m_library{};
    Buffer m_image{};
    u8 m_libraryQueue = rg::kNoQueue;
    u8 m_imageQueue = rg::kNoQueue;
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    Buffer m_resolveTable{};
    u8 m_resolveTableQueue = rg::kNoQueue;
    BindlessSlotHandle m_resolveTableSlot{};
    u32 m_resolveTableHandle = 0;
    std::vector<Texture> m_resolveImages;
    std::vector<BindlessSlotHandle> m_resolveImageSlots;
    std::vector<u32> m_resolveImageHandles;
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr;
    void* m_pipelines[kKernelCount] = {};
    MaterialLayerStats m_stats{};
};

} // namespace fuse::renderer::material_layers
