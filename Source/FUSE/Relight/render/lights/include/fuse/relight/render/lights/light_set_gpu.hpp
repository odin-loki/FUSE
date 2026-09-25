// FUSE Relight RL-4.4: the Relight light set on the GPU (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1 "GPU scene update
// (lights, emissive tris)" and "light tree build/refit").
//
// Per frame, one host-visible ring slot (framesInFlight slots, buffer device addresses) holds the raw D3DLIGHT9
// records of the frame's game lights and the frame's light table (RlLight, kRlLightWords float4 each; tree light
// index == table index). The host writes the authored / emissive part of the table; the "relight.lights.convert"
// pass writes the game-light part on the GPU from the raw records (the D3D fixed-function light conversion runs on the
// GPU; the CPU runs the same single-source kernel for the tree). The WP-7.1 tree goes through LightTreeGpu (its own
// ring, uploaded only when the tree version changes).
//
//   gpu.beginFrame(serial, set);                           // set.build() done
//   RelightLightsGraphRefs refs = gpu.importInto(graph);
//   gpu.addConvertPass(graph, refs);                       // writes table[0, gameLightCount)
//   consumer pass: .use(refs.ring, StorageRead, refs.tableRange, stage) and .use(refs.tree.tree, StorageRead,
//                  refs.tree.range, stage); push refs.tableAddress, refs.lightCount, refs.tree.header; include
//                  render/lights/shaders/rl_lights.{slang,glsl} (rlSetSample / rlSetPdf / rlLoadLight).
//   gpu.addSamplePass(graph, refs, queries, ..., results, ..., count);   // batch sampler (tests, debug views)
//   gpu.collectRetired(completedSerial);
//
// Steady-state frames (no more lights than an earlier frame) make no heap allocation.
#pragma once

#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/renderer/light_tree/light_tree_gpu.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::relight::render::lights {

enum class LightKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct RelightLightsGpuDesc {
    renderer::VulkanDevice* device = nullptr;
    renderer::GpuAllocator* allocator = nullptr;
    /// Bound for the passes (descriptor-buffer backend: pipelines carry its create flags); the kernels read only
    /// buffer device addresses.
    renderer::BindlessDescriptors* bindless = nullptr;
    LightKernelLanguage language = LightKernelLanguage::Auto;
    u32 framesInFlight = 3;
    u32 initialLights = 256;
};

/// Byte layout of one ring slot (sections 256-aligned).
struct RelightLightsSlotLayout {
    u64 d3d = 0;   ///< float4[kRlD3dWords x gameLights]
    u64 table = 0; ///< float4[kRlLightWords x lights]
    u64 bytes = 0;
    static RelightLightsSlotLayout compute(u32 gameLights, u32 lights);
};

struct RelightLightsGraphRefs {
    renderer::rg::BufferRef ring;
    renderer::rg::BufferRange d3dRange{};
    renderer::rg::BufferRange tableRange{};
    u64 d3dAddress = 0;
    u64 tableAddress = 0;
    u32 lightCount = 0;
    u32 gameLightCount = 0;
    renderer::light_tree::LightTreeGraphRefs tree{};
};

struct RelightLightsGpuStats {
    u32 frames = 0;
    u64 uploadBytes = 0; ///< cumulative host writes into the ring (raw D3D + host table part)
    u32 reallocations = 0;
    u32 convertPasses = 0; ///< this frame
    u32 samplePasses = 0;  ///< this frame
};

class RelightLightsGpu {
public:
    RelightLightsGpu() = default;
    ~RelightLightsGpu();
    RelightLightsGpu(const RelightLightsGpu&) = delete;
    RelightLightsGpu& operator=(const RelightLightsGpu&) = delete;

    /// Fails (false, nothing created) without a capable device (BDA + int64), without a built kernel of the
    /// requested language, or in the stub backend.
    bool init(const RelightLightsGpuDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }

    /// Selects this frame's slot, writes the raw game lights and the host part of the table, and brings the tree ring
    /// up to date. False on allocation failure.
    bool beginFrame(u64 frameSerial, const RelightLightSet& set);
    RelightLightsGraphRefs importInto(renderer::rg::Graph& graph);
    /// "relight.lights.convert" (no pass when the frame has no game light; true then).
    bool addConvertPass(renderer::rg::Graph& graph, const RelightLightsGraphRefs& refs);
    /// "relight.lights.sample": kRlQueryWords float4 per query at queriesAddress -> kRlResultWords per result.
    bool addSamplePass(renderer::rg::Graph& graph, const RelightLightsGraphRefs& refs, renderer::rg::BufferRef queries,
                       u64 queriesOffset, u64 queriesAddress, renderer::rg::BufferRef results, u64 resultsOffset,
                       u64 resultsAddress, u32 count);
    u32 collectRetired(u64 completedSerial);

    const char* kernelLanguage() const { return m_language; }
    u64 tableAddress() const { return m_tableAddress; }
    u64 treeHeaderAddress() const { return m_tree.headerAddress(); }
    /// This frame's table in the (host-visible) ring: readable after the frame completed (tests).
    const void* mappedTable() const;
    const RelightLightsSlotLayout& slotLayout() const { return m_layout; }
    const RelightLightsGpuStats& stats() const { return m_stats; }
    const renderer::light_tree::LightTreeGpu& treeGpu() const { return m_tree; }

private:
    struct Retired {
        renderer::Buffer buffer{};
        u64 serial = 0;
    };
    struct ConvertPush {
        u64 source = 0;
        u64 table = 0;
        u32 count = 0;
        u32 leastSquares = 0;
        f32 sphereRadius = 0.f;
        f32 intensityFactor = 0.f;
        f32 maxIntensity = 0.f;
        f32 distantIntensity = 0.f;
        f32 distantAngle = 0.f;
        u32 reserved = 0;
    };
    struct SamplePush {
        u64 table = 0;
        u64 tree = 0;
        u64 queries = 0;
        u64 results = 0;
        u32 count = 0;
        u32 lightCount = 0;
        u32 reserved0 = 0;
        u32 reserved1 = 0;
    };
    static_assert(sizeof(ConvertPush) == 48u && sizeof(SamplePush) == 48u, "push constants (rl_light_*.comp / .slang)");
    struct PassRecord {
        RelightLightsGpu* self = nullptr;
        bool convert = false;
        ConvertPush convertPush{};
        SamplePush samplePush{};
        u32 groups = 1;
    };
    static constexpr u32 kMaxPasses = 16u;
    static constexpr u32 kMaxSlots = 8u;

    bool createPipelines();
    bool ensureCapacity(u64 slotBytes);
    static void recordDispatch(const renderer::rg::PassContext& context, void* user);

    RelightLightsGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    renderer::light_tree::LightTreeGpu m_tree;
    u64 m_frameSerial = 0;
    renderer::Buffer m_ring{};
    u8 m_ringQueue = renderer::rg::kNoQueue;
    u64 m_slotStride = 0;
    u32 m_slot = 0;
    RelightLightsSlotLayout m_layout{};
    u64 m_tableAddress = 0;
    u32 m_lightCount = 0;
    u32 m_gameCount = 0;
    lk::RlConvertParams m_params{};
    std::vector<Retired> m_retired;
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr;     ///< VkPipelineLayout (one push range, 48 bytes)
    void* m_convertPipeline = nullptr;  ///< VkPipeline
    void* m_samplePipeline = nullptr;   ///< VkPipeline
    RelightLightsGpuStats m_stats{};
};

} // namespace fuse::relight::render::lights
