// FUSE Relight RL-5.3: ReSTIR GI on the GPU - the stages of restir_gi_core.h as render-graph (WP-0.3 v2) compute
// passes on PathTracerGpu's scene, recorded before "relight.pt.trace" (after RL-5.2's DI passes when both run):
//
//   relight.lights.convert                (PathTracerGpu::addLightsPass, so the passes below read this frame's table)
//   relight.restir_gi.surface             surfaces[cur] <- the path tracer's primary chain (record mode)
//   relight.restir_gi.initial             initial <- one BSDF continuation traced to x2 + its tail, the residual
//   relight.restir_gi.temporal            A <- initial + history at the reprojected pixel (surfaces[prev])
//   relight.restir_gi.spatial (x N)       A <-> B
//   relight.restir_gi.shade               output (key, indirect), history <- final
//   relight.pt.trace                      reads output (PathTracerGpu::setRestirGi; kPtFlagRestirGi in the settings)
//
// Every pass declares its accesses (the TLAS, the path tracer's tables and ring slot, the light ring / tree, this
// class's buffers); no manual barriers. Buffers: persistent host-visible sections (surfaces x 2, reservoirs x 4,
// output; the tests read them back) and a per-frame ring slot (the stage block: addresses + params). Steady-state
// frames (same size) make no heap allocation.
//
//   gi.init(desc);
//   pt.setScene(c); pt.beginFrame(serial, c, frame);          // frame.settings.flags |= kPtFlagRestirGi
//   gi.beginFrame(serial, c, frame, settings);
//   refs = pt.importInto(graph); gi.addPasses(graph, pt, refs); pt.addTracePass(graph, refs);
#pragma once

#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/pathtrace/restir_gi.hpp>

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::relight::render::pathtrace {

struct RestirGiGpuDesc {
    renderer::VulkanDevice* device = nullptr;
    renderer::GpuAllocator* allocator = nullptr;
    renderer::BindlessDescriptors* bindless = nullptr; ///< required (the path tracer's pipeline layout)
    PtKernelLanguage language = PtKernelLanguage::Auto;
    u32 framesInFlight = 3;
};

/// Persistent sections (RestirGiGpu::mappedSection).
enum RestirGiSection : u32 {
    kRgiSecSurface0 = 0,
    kRgiSecSurface1 = 1,
    kRgiSecInitial = 2,
    kRgiSecA = 3,
    kRgiSecB = 4,
    kRgiSecHistory = 5,
    kRgiSecOutput = 6,
    kRgiSections = 7,
};

struct RestirGiGraphRefs {
    renderer::rg::BufferRef sections[kRgiSections];
    renderer::rg::BufferRef ring;
    renderer::rg::BufferRange ringRange{};
    bool valid = false;
};

struct RestirGiGpuStats {
    u32 frames = 0;
    u32 passes = 0; ///< this frame
    u32 reallocations = 0;
};

class RestirGiGpu {
public:
    RestirGiGpu();
    ~RestirGiGpu();
    RestirGiGpu(const RestirGiGpu&) = delete;
    RestirGiGpu& operator=(const RestirGiGpu&) = delete;

    bool init(const RestirGiGpuDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// This frame's stage block and buffers (after PathTracerGpu::beginFrame of the same frame).
    bool beginFrame(u64 frameSerial, const PtCompiledScene& scene, const PtFrameDesc& frame,
                    const RestirGiSettings& settings);
    /// The passes above, and PathTracerGpu::setRestirGi for its trace pass (add it afterwards).
    bool addPasses(renderer::rg::Graph& graph, PathTracerGpu& pt, const PtGraphRefs& ptRefs);
    const RestirGiGraphRefs& graphRefs() const { return m_refs; }
    u32 collectRetired(u64 completedSerial);
    void resetHistory() { m_historyValid = false; }

    /// Read-back (after the frame completed): a section's words, this frame's surface slot, the final reservoir
    /// section (A or B), the flags the frame ran with.
    const void* mappedSection(u32 section) const;
    u32 currentSurfaceSection() const { return m_cur == 0u ? kRgiSecSurface0 : kRgiSecSurface1; }
    u32 finalSection() const { return m_final; }
    u32 frameFlags() const { return m_flags; }
    const RestirGiGpuStats& stats() const { return m_stats; }

private:
    struct Push {
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
        u64 restirDi = 0;
        u64 block = 0;
        u64 src = 0;
        u64 dst = 0;
        u32 width = 0;
        u32 height = 0;
        u32 lightCount = 0;
        u32 stage = 0;
    };
    static_assert(sizeof(Push) == 128u, "RgiPush (restir_gi.comp / .slang)");
    struct PassRecord {
        RestirGiGpu* self = nullptr;
        Push push{};
        u32 groupsX = 1;
        u32 groupsY = 1;
    };
    struct Retired {
        renderer::Buffer buffer{};
        u64 serial = 0;
    };
    static constexpr u32 kMaxPasses = 12u;
    static constexpr u32 kMaxSlots = 8u;

    bool createPipeline();
    bool ensureBuffer(renderer::Buffer& buffer, u64 bytes, u32 memory, const char* name, u8* queue);
    static void record(const renderer::rg::PassContext& context, void* user);

    RestirGiGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    RestirGiSettings m_settings{};
    renderer::Buffer m_sections[kRgiSections]{};
    u8 m_sectionQueues[kRgiSections] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
    u64 m_sectionBytes[kRgiSections] = {};
    renderer::Buffer m_ring{};
    u8 m_ringQueue = 0xFFu;
    u64 m_blockOffset = 0;
    std::vector<Retired> m_retired;
    PassRecord m_records[kMaxPasses]{};
    RestirGiGraphRefs m_refs{};
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_cur = 0;
    u32 m_final = kRgiSecA;
    u32 m_flags = 0;
    bool m_historyValid = false;
    bool m_frameReady = false;
    void* m_layoutHandle = nullptr;
    void* m_pipeline = nullptr;
    RestirGiGpuStats m_stats{};
};

} // namespace fuse::relight::render::pathtrace
