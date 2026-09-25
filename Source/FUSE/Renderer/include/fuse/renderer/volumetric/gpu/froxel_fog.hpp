#pragma once

// WP-8.1 froxel volumetric fog on Vulkan (renderer plan Phase 8 "froxel volumetric fog with temporal
// reprojection, lit by clustered lights, shadows and GI"; execution doc WP-8.1). Compute passes on render
// graph v2 over a view-aligned froxel grid (the B5 FroxelGridDesc layout and exponential slices, sized
// independently of the WP-2.1 clusters but aligned with them when the counts divide):
//
//   fog.beginFrame(serial, settings, {camera, lighting.frameConstantsAddress(), shadows, &rt4, &lit});
//   FogGraphRefs f = fog.importInto(graph);
//   // after lighting.addAssignment (and VsmShadows::addSamplingUse(.., rg::kStageCompute) for shadows)
//   fog.addPasses(graph, f, {lightingRefs.lists, &sceneRefs, rt4Ref, litRef});
//   // f.output: the lit image with fog applied (RGBA16F); integratedAddress() for other consumers
//
// Passes (all compute, all declared on the graph, no manual barriers; froxel data in one persistent work
// buffer reached through BDA; CPU reference: froxel_fog_kernel.hpp, one function per thread):
//   fog.inject     one thread per froxel: jittered sample point, height fog + local volumes, in-scattering
//                  from ambient + the WP-2.1 directional list + the sample's cluster list (point / spot, the
//                  oracle's falloff / cone) x Henyey-Greenstein x VSM visibility (WP-3.2, when given)
//   fog.temporal   one thread per froxel: reprojects the froxel centre into the previous camera's volume,
//                  trilinear history, exponential blend (alpha); the current sample where it was outside
//   fog.integrate  one thread per froxel column: front-to-back energy-conserving integration from the camera
//                  -> (in-scattering, transmittance) at each slice's far boundary
//   fog.apply      8 x 8 tiles: RT4 depth -> the volume at that depth (piecewise linear in depth, bilinear
//                  across columns); out = lit x transmittance + in-scattering (+ optional f32x4 dump)
// Kernels: Slang primary (-fp-mode precise) with GLSL twins (`precise`), embedded by cmake/rp_wp81.cmake.
// Steady-state frames make no heap allocation (the work buffer follows the grid, the output the extent;
// input bindings change only when the images do).

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_reference.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::volumetric_gpu {

enum class FogKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct FogCapabilities {
    bool fog = false;
    const char* reason = "no device"; ///< "ok" when usable
};

FogCapabilities queryFogCapabilities(const VulkanDevice* device);

/// Byte layout of the work buffer for a grid (every section 256-aligned, f32x4 per froxel).
struct FogBufferLayout {
    u64 current = 0;
    u64 history[2] = {0, 0};
    u64 integrated = 0;
    u64 workBytes = 0;

    static FogBufferLayout compute(u32 froxelCount);
};

struct FroxelFogDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    FogKernelLanguage language = FogKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant ring slots
};

struct FogFrameDesc {
    /// The camera the depth was rendered with (normally LightingFrameDesc::camera): position / basis /
    /// fovY / aspect, near / far and the depth convention (RT4 = forward z/w: reversedZ = false).
    ClusterCameraDesc camera{};
    /// ClusteredLighting::frameConstantsAddress() of this frame: the fog reads its camera / grid / light
    /// count, the scene light table and the cluster lists (declare FogGraphInputs::lights / scene). 0 = the
    /// medium is lit by the ambient term only.
    u64 lighting = 0;
    /// VsmShadows::shadowConstantsAddress() (WP-3.2): each light x its visibility at the sample (declare the
    /// images with VsmShadows::addSamplingUse(.., rg::kStageCompute) before addPasses). 0 = unshadowed.
    u64 shadows = 0;
    /// fog.apply inputs (same extent; both null = no apply pass): RT4 (R32F device depth) and the lit
    /// image (RGBA16F), bindless sampled.
    const Texture* depth = nullptr;
    const Texture* lit = nullptr;
    /// Optional f32x4-per-pixel buffer (BDA) receiving the applied colour before RGBA16F rounding.
    u64 dumpAddress = 0;
    /// Drops the history (camera cut); the next frame starts accumulating again.
    bool resetHistory = false;
};

/// Graph refs of the inputs (as imported by their owners).
struct FogGraphInputs {
    rg::BufferRef lights;                            ///< LightingGraphRefs::lists (with FogFrameDesc::lighting)
    const gpu_scene::GpuSceneGraphRefs* scene = nullptr; ///< the scene's refs (the light table, BDA)
    rg::TextureRef depth;
    rg::TextureRef lit;
    rg::BufferRef dump;
};

struct FogGraphRefs {
    rg::BufferRef work;
    rg::TextureRef output; ///< fogged RGBA16F image (invalid without apply inputs)
};

/// Work-buffer section an addCopy reads (gates / debugging).
enum class FogCopySource : u8 {
    Current = 0,  ///< fog.inject output
    History,      ///< this frame's fog.temporal output
    HistoryPrev,  ///< the history fog.temporal read this frame
    Integrated,   ///< fog.integrate output
};

struct FogStats {
    u32 workRebuilds = 0;
    u32 outputRebuilds = 0;
    u32 inputBinds = 0;
    u32 retired = 0;
    u32 passes = 0;        ///< passes added this frame
    bool historyUsed = false; ///< this frame blended a history
    bool applied = false;
};

class FroxelFog {
public:
    FroxelFog() = default;
    ~FroxelFog();
    FroxelFog(const FroxelFog&) = delete;
    FroxelFog& operator=(const FroxelFog&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the requested
    /// language, or in the stub backend.
    bool init(const FroxelFogDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Resolves and writes this frame's constants (ring slot of `frameSerial`); follows the grid (work
    /// buffer; a new grid drops the history) and the apply extent / images. False on an invalid camera or
    /// grid, mismatched apply inputs or a failed (re)allocation.
    bool beginFrame(u64 frameSerial, const FroxelFogSettings& settings, const FogFrameDesc& frame);
    /// Rewinds the inject jitter sequence (Halton index) to its start, so the next frame is a function of its
    /// inputs alone (FogFrameDesc::resetHistory drops the history but keeps the sequence running).
    void resetSequence() { m_frameIndex = 0; }

    FogGraphRefs importInto(rg::Graph& graph);
    /// fog.inject, fog.temporal, fog.integrate, fog.apply (with apply inputs).
    void addPasses(rg::Graph& graph, const FogGraphRefs& refs, const FogGraphInputs& inputs);
    /// Copies a work-buffer section (froxelCount x 16 bytes) into `dst` at `dstOffset` (after addPasses).
    void addCopy(rg::Graph& graph, const FogGraphRefs& refs, FogCopySource what, rg::BufferRef dst, u64 dstOffset);
    u64 copyBytes() const { return static_cast<u64>(m_constants.froxelCount) * 16u; }

    /// Destroys buffers / images / bindless slots retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection / consumers -----------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const FogFrameConstants& constants() const { return m_constants; }
    const FogBufferLayout& layout() const { return m_layout; }
    const FogStats& stats() const { return m_stats; }
    const Texture& outputImage() const { return m_output.image; }
    const Buffer& workBuffer() const { return m_work; }
    /// BDA of this frame's FogFrameConstants (grid, camera, slice table): consumers sampling the volume
    /// with fuse_fog_sample (fog_common.{glsl,slang}) read it together with integratedAddress().
    u64 frameConstantsAddress() const { return m_frameAddress; }
    /// BDA of the integrated volume (valid after fog.integrate of this frame; declare a StorageRead of
    /// FogGraphRefs::work).
    u64 integratedAddress() const { return m_constants.integrated; }

private:
    struct Output {
        Texture image{};
        BindlessSlotHandle slot{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    static constexpr u32 kInputCount = 2u; ///< RT4, lit
    struct Retired {
        Buffer buffer{};
        Texture image{};
        BindlessSlotHandle slots[1u + kInputCount]{};
        u64 serial = 0;
    };
    struct PassRecord {
        FroxelFog* self = nullptr;
        FogPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
        u64 copySrc = 0;
        u64 copyDst = 0;
        u64 copyBytes = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    enum Kernel : u32 { kInject = 0, kTemporal, kIntegrate, kApply, kKernelCount };
    static constexpr u32 kMaxPasses = 16u;

    bool createPipelines();
    bool createWork(u32 froxelCount);
    bool createOutput(u32 width, u32 height);
    bool bindInput(u32 index, const Texture* image);
    void retire(Retired r);
    void destroyRetired(Retired& r);
    PassRecord* nextRecord(u32 kernel);
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    FroxelFogDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_frameIndex = 0;
    u32 m_froxels = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    FogBufferLayout m_layout{};
    Buffer m_work{};
    u8 m_workQueue = rg::kNoQueue;
    Output m_output{};
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    FogFrameConstants m_constants{};
    bool m_hasDump = false;
    bool m_apply = false;

    // History: the section written last frame, its grid / range, the camera it was rendered with.
    u32 m_historyIndex = 0;
    bool m_historyValid = false;
    ClusterCameraDesc m_prevCamera{};
    u32 m_prevGrid[3] = {0u, 0u, 0u};
    f32 m_prevRange[2] = {0.f, 0.f};

    void* m_inputImages[kInputCount] = {};
    BindlessSlotHandle m_inputSlots[kInputCount]{};
    u32 m_inputHandles[kInputCount] = {};

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push, compute)
    void* m_pipelines[kKernelCount] = {};

    FogStats m_stats{};
};

} // namespace fuse::renderer::volumetric_gpu
