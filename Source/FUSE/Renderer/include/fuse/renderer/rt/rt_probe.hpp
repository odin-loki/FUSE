#pragma once

// WP-6.0 ray-query compute probe: one rayQueryEXT per RtProbeRay against a TLAS, closest committed
// triangle hit written as an RtProbeHit (rt_types.hpp). It is the exit-test instrument of the
// acceleration-structure builder (hit ids and t against the CPU BVH reference, rt_reference.hpp) and the
// smallest ray-query consumer later T2 passes (WP-6.1 DDGI probe rays, WP-6.2 RT shadows) copy.
//
//   RtProbe probe; probe.init(device);                          // T2 gate: false on T0 / T1
//   probe.addPass(graph, RtProbeDispatch{rtRefs.tlas, rt.tlasAddress(), raysRef, raysAddr, hitsRef, hitsAddr, n});
//
// Pure BDA: rays / hits are device addresses, the TLAS is reached through its device address
// (accelerationStructureEXT(uint64_t)); the pass declares AccelerationStructureRead on the TLAS,
// StorageRead on the rays and StorageWrite on the hits (compute stage), so the graph orders it after
// rt.tlas.build. Up to kMaxDispatches dispatches per frame; no heap allocation after init().

#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/renderer/rt/rt_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class VulkanDevice;
}

namespace fuse::renderer::rt {

struct RtProbeDispatch {
    rg::BufferRef tlas;
    u64 tlasAddress = 0;
    rg::BufferRef rays;
    u64 raysAddress = 0; ///< RtProbeRay[count]
    rg::BufferRef hits;
    u64 hitsAddress = 0; ///< RtProbeHit[count]
    u32 count = 0;
    u32 cullMask = kRtMaskAll;
    u32 rayFlags = 0;
};

class RtProbe {
public:
    static constexpr u32 kMaxDispatches = 8;

    RtProbe() = default;
    ~RtProbe();
    RtProbe(const RtProbe&) = delete;
    RtProbe& operator=(const RtProbe&) = delete;

    /// `bindless`: the frame's heap when the graph also runs bindless passes (the descriptor-free pipeline
    /// then carries its pipelineCreateFlags(); see AccelerationStructuresDesc::bindless). Null = none.
    bool init(VulkanDevice* device, RtKernelLanguage language = RtKernelLanguage::Auto, BindlessDescriptors* bindless = nullptr);
    void destroy();
    bool ready() const { return m_pipeline != nullptr; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// Adds "rt.probe" (compute). Returns false when not ready, the dispatch is empty or the frame's
    /// dispatch slots are used up. Call beginFrame() once per graph.
    bool addPass(rg::Graph& graph, const RtProbeDispatch& dispatch);
    void beginFrame() { m_count = 0; }

private:
    static void record(const rg::PassContext& context, void* user);

    VulkanDevice* m_device = nullptr;
    void* m_pipeline = nullptr; ///< VkPipeline
    void* m_layout = nullptr;   ///< VkPipelineLayout
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    /// One dispatch recorded by "rt.probe" (the pass callback's user pointer).
    struct Slot {
        RtProbe* owner = nullptr;
        RtProbePush push{};
    };
    Slot m_slots[kMaxDispatches] = {};
    u32 m_count = 0;
};

} // namespace fuse::renderer::rt
