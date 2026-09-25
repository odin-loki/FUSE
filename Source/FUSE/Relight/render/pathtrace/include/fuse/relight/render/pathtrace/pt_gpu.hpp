// FUSE Relight RL-5.1: the path tracer on the GPU (tier T2: ray queries; docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8,
// §5.1). One compute pass, "relight.pt.trace", runs the single-source core (kernels/pt_reference_core.h) per pixel
// against the WP-6.0 acceleration structures of a PtCompiledScene:
//
//   geometry   PathTracerGpu owns a WP-1.1 GpuScene holding the scene's cooked meshlet meshes (addMeshletMesh: the
//              scene index buffer + VPOS streams the BLAS build decodes) and one instance per PtInstance, and a WP-6.0
//              AccelerationStructures over it (BLAS per mesh, TLAS over the instance slots: instanceCustomIndex =
//              slot, which the compiled scene's instance table is indexed by - setScene() hands the slots back);
//   lights     RL-4.4 RelightLightsGpu (ring slot with the light table + the WP-7.1 tree), the same set the CPU
//              reference samples;
//   tables     a static buffer (triangle words, the RL-4.3 albedo table) written when the scene structure changes,
//              and a per-frame ring slot (params, instance / material / portal words, light map);
//   outputs    one host-visible buffer of kPtOutSections sections (PtOutputSection) for the frame's radiance, the
//              accumulation sums (accumulation reference mode: PtFrameDesc::accumulate adds this frame's sums to the
//              previous ones), the demodulated channels and the G-buffer - the WP-6.4 denoiser's inputs by device
//              address (signal f32x4, motion f32x2, linear depth f32, normal f32x4).
//
//   gpu.setScene(compiled);                       // structure change: waits for nothing - the caller retired every
//                                                 // frame that used the previous structure (see setScene)
//   gpu.beginFrame(serial, compiled, frame);      // GpuScene / AS / lights / ring for this frame, then upload.flush()
//   PtGraphRefs refs = gpu.importInto(graph);     // scene, AS passes (rt.*), lights, tables, outputs
//   gpu.addTracePass(graph, refs);                // "relight.pt.trace"
//   ... executor.execute(graph); gpu.collectRetired(completedSerial);
//
// Steady-state frames (same structure: transforms, lights, camera, settings change) make no heap allocation.
// Requires the WP-6.0 T2 gate (acceleration structures + ray query), BDA + int64, the WP-7.1 light tree GPU path and
// the bindless heap (textures); init() fails otherwise (and always in the stub backend).
#pragma once

#include <fuse/relight/render/lights/light_set_gpu.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::relight::render::pathtrace {

enum class PtKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

/// Output sections (per pixel, row-major).
enum PtOutputSection : u32 {
    kPtOutRadiance = 0,  ///< f32x4: this frame's mean radiance, samples
    kPtOutAccum = 1,     ///< f32x4: accumulated radiance sum, sample count
    kPtOutAccumSq = 2,   ///< f32x4: accumulated sum of squares, 0
    kPtOutEmissive = 3,  ///< f32x4: mean emissive (primary chain) radiance, 0
    kPtOutDiffuse = 4,   ///< f32x4: mean demodulated diffuse radiance, hit distance (diffuse continuation)
    kPtOutSpecular = 5,  ///< f32x4: mean demodulated specular radiance, hit distance (specular continuation)
    kPtOutAlbedoD = 6,   ///< f32x4: diffuse albedo (with the PSR chain throughput), PSR chain length
    kPtOutAlbedoS = 7,   ///< f32x4: specular albedo, sample flags
    kPtOutNormal = 8,    ///< f32x4: G-buffer normal (world), perceptual roughness
    kPtOutMotion = 9,    ///< f32x2: UV motion (previous - current)
    kPtOutDepth = 10,    ///< f32: view depth (0: sky)
    kPtOutInstance = 11, ///< u32: GPU-scene instance slot (~0: sky)
    kPtOutSections = 12,
};

struct PathTracerGpuDesc {
    renderer::VulkanDevice* device = nullptr;
    renderer::GpuAllocator* allocator = nullptr;
    renderer::UploadQueue* upload = nullptr;
    renderer::BindlessDescriptors* bindless = nullptr; ///< required (textures; pipeline create flags)
    PtKernelLanguage language = PtKernelLanguage::Auto;
    u32 framesInFlight = 3;
};

struct PtFrameDesc {
    u32 width = 0;
    u32 height = 0;
    u32 frameSeed = 0;
    u32 sampleBase = 0;      ///< first sample index of this frame
    bool accumulate = false; ///< add to the accumulation sections (false: start them over)
    PtSettings settings{};
};

struct PtGraphRefs {
    renderer::gpu_scene::GpuSceneGraphRefs scene{};
    renderer::rt::RtGraphRefs rt{};
    lights::RelightLightsGraphRefs lights{};
    renderer::rg::BufferRef statics;
    renderer::rg::BufferRef ring;
    renderer::rg::BufferRange ringRange{};
    renderer::rg::BufferRef outputs; ///< declare HostRead to read back after the frame
    bool valid = false;
};

/// The device addresses "relight.pt.trace" pushes (RL-5.2's ReSTIR DI passes run the same core on them).
struct PtTraceBindings {
    u64 params = 0;
    u64 instances = 0;
    u64 triangles = 0;
    u64 materials = 0;
    u64 portals = 0;
    u64 lightMap = 0;
    u64 lightTable = 0;
    u64 lightTree = 0;
    u64 tlas = 0;
    u64 lut = 0;
    u32 lightCount = 0;
    u32 width = 0;
    u32 height = 0;
};

struct PathTracerGpuStats {
    u32 frames = 0;
    u32 sceneBuilds = 0;     ///< structural (re)builds
    u32 tracePasses = 0;     ///< this frame
    u32 reallocations = 0;   ///< ring / output growth
    u64 uploadBytes = 0;     ///< cumulative host writes into the ring
};

class PathTracerGpu {
public:
    PathTracerGpu();
    ~PathTracerGpu();
    PathTracerGpu(const PathTracerGpu&) = delete;
    PathTracerGpu& operator=(const PathTracerGpu&) = delete;

    /// False (nothing created, reason()) without the requirements above.
    bool init(const PathTracerGpuDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// Makes `scene` the traced scene. When its structure (meshes, instance -> mesh) differs from the current one
    /// the GpuScene / acceleration structures are rebuilt (the caller must have retired every frame that used the
    /// previous structure) and the instance slots are handed to `scene` (setSlots). Otherwise the instances move.
    /// Call once per frame before beginFrame (after scene.update()).
    bool setScene(PtCompiledScene& scene);
    /// The next setScene rebuilds the GPU scene / acceleration structures (a scene recompiled with new geometry of the
    /// same structure; the caller retired every frame that used the current one).
    void invalidateScene() { m_sceneReady = false; }
    /// This frame's scene commits, lights and ring slot (flush the UploadQueue afterwards, before executing).
    bool beginFrame(u64 frameSerial, const PtCompiledScene& scene, const PtFrameDesc& frame);
    PtGraphRefs importInto(renderer::rg::Graph& graph);
    bool addTracePass(renderer::rg::Graph& graph, const PtGraphRefs& refs);
    /// RL-5.2 hooks. addLightsPass: "relight.lights.convert" now (once per frame; addTracePass then skips it), so
    /// passes recorded before the trace can read the light table. traceBindings: the trace pass's addresses (valid
    /// after beginFrame). setRestirDi: the next addTracePass reads ReSTIR DI's per-pixel output at `address`
    /// (declared StorageRead on `buffer` / `range`); beginFrame clears it.
    bool addLightsPass(renderer::rg::Graph& graph, const PtGraphRefs& refs);
    PtTraceBindings traceBindings() const;
    void setRestirDi(u64 address, renderer::rg::BufferRef buffer, renderer::rg::BufferRange range);
    /// RL-5.3: the next addTracePass reads ReSTIR GI's per-pixel output (as setRestirDi); beginFrame clears it.
    void setRestirGi(u64 address, renderer::rg::BufferRef buffer, renderer::rg::BufferRange range);
    u32 collectRetired(u64 completedSerial);

    /// Output buffer (host-visible; valid after the frame completed) and a section's first byte.
    const void* mappedOutputs() const;
    u64 outputAddress(u32 section) const;
    u64 outputStride() const { return m_outStride; }
    const renderer::Buffer& outputBuffer() const { return m_outputs; }
    const renderer::rt::AccelerationStructures& accelerationStructures() const { return *m_as; }
    const renderer::gpu_scene::GpuScene& gpuScene() const { return *m_scene; }
    const PathTracerGpuStats& stats() const { return m_stats; }

private:
    struct Retired {
        renderer::Buffer buffer{};
        u64 serial = 0;
    };
    struct TracePush {
        u64 params = 0;
        u64 instances = 0;
        u64 triangles = 0;
        u64 materials = 0;
        u64 portals = 0;
        u64 lightMap = 0;
        u64 lightTable = 0;
        u64 lightTree = 0;
        u64 tlas = 0;
        u64 lut = 0;
        u64 outputs = 0;
        u64 outStride = 0;
        u32 width = 0;
        u32 height = 0;
        u32 accumulate = 0;
        u32 lightCount = 0;
        u64 restirDi = 0; ///< RL-5.2: RestirDiGpu's per-pixel DI output (0: off)
        u64 restirGi = 0; ///< RL-5.3: RestirGiGpu's per-pixel GI output (0: off)
    };
    static_assert(sizeof(TracePush) == 128u, "PtTracePush (rl_pt_trace.comp / .slang)");
    struct PassRecord {
        PathTracerGpu* self = nullptr;
        TracePush push{};
        u32 groupsX = 1;
        u32 groupsY = 1;
    };
    struct RingLayout {
        u64 params = 0;
        u64 instances = 0;
        u64 materials = 0;
        u64 portals = 0;
        u64 lightMap = 0;
        u64 bytes = 0;
    };
    static constexpr u32 kMaxSlots = 8u;

    bool createPipeline();
    bool rebuildScene(PtCompiledScene& scene);
    bool ensureBuffer(renderer::Buffer& buffer, u64 bytes, u32 usage, u32 memory, const char* name, u8* queue);
    static void recordTrace(const renderer::rg::PassContext& context, void* user);
    void releaseScene();

    PathTracerGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    std::unique_ptr<renderer::gpu_scene::GpuScene> m_scene;
    std::unique_ptr<renderer::rt::AccelerationStructures> m_as;
    lights::RelightLightsGpu m_lights;
    std::vector<renderer::gpu_scene::InstanceHandle> m_handles;
    std::vector<u32> m_meshSignature; ///< per mesh triangle count, then per instance its mesh
    bool m_sceneReady = false;
    u64 m_frameSerial = 0;

    renderer::Buffer m_statics{};
    u8 m_staticsQueue = 0xFFu;
    u64 m_trianglesOffset = 0;
    u64 m_lutOffset = 0;
    renderer::Buffer m_ring{};
    u8 m_ringQueue = 0xFFu;
    u64 m_slotStride = 0;
    u32 m_slot = 0;
    RingLayout m_ringLayout{};
    renderer::Buffer m_outputs{};
    u8 m_outputsQueue = 0xFFu;
    u64 m_outStride = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_accumulate = false;
    u32 m_lightCount = 0;
    bool m_lightsAdded = false;
    u64 m_restirDiAddress = 0;
    renderer::rg::BufferRef m_restirDiBuffer{};
    renderer::rg::BufferRange m_restirDiRange{};
    u64 m_restirGiAddress = 0;
    renderer::rg::BufferRef m_restirGiBuffer{};
    renderer::rg::BufferRange m_restirGiRange{};
    std::vector<Retired> m_retired;
    PassRecord m_record{};
    void* m_layoutHandle = nullptr;   ///< VkPipelineLayout
    void* m_pipeline = nullptr;       ///< VkPipeline
    PathTracerGpuStats m_stats{};
};

} // namespace fuse::relight::render::pathtrace
