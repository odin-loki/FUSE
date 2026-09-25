// FUSE Relight RL-5.4: the hash-grid radiance cache on the GPU - the stages of kernels/radiance_cache_core.h /
// radiance_cache_path.h as render-graph (WP-0.3 v2) compute passes on PathTracerGpu's scene, recorded before
// "relight.pt.trace":
//
//   relight.lights.convert              (PathTracerGpu::addLightsPass: the training paths read this frame's lights)
//   relight.radiance_cache.clear        table <- 0 (first frame, reallocation, reset())
//   relight.radiance_cache.train        records <- one training path per tile (the path tracer's core, kPtFlagRcTrain);
//                                       table header <- this frame's RcParams, counters 0
//   relight.radiance_cache.update       table <- records (insert + integer-atomic accumulation)
//   relight.radiance_cache.resolve      table <- temporal blend, age, evict
//   relight.radiance_cache.probe        (enableProbe: gates) probe <- the cache's answer at every training record
//   relight.pt.trace                    queries the table (PathTracerGpu::setRadianceCache; kPtFlagRadianceCache)
//
// One pipeline (shaders/radiance_cache.{slang,comp}, the stage in the push constants). Every pass declares its
// accesses; no manual barriers. Buffers: the table and the records are persistent host-visible buffers (the gates read
// them back), the ring holds one 256-byte stage block per frame in flight (table / records / probe addresses +
// RcParams at byte 32).
// Steady-state frames (same size and capacity) make no heap allocation.
//
//   rc.init(desc);
//   pt.setScene(c); pt.beginFrame(serial, c, frame);        // frame.settings.flags |= kPtFlagRadianceCache
//   rc.beginFrame(serial, c, frame, settings);
//   refs = pt.importInto(graph); rc.addPasses(graph, pt, refs); pt.addTracePass(graph, refs);
#pragma once

#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/pathtrace/radiance_cache.hpp>

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

struct RadianceCacheGpuDesc {
    renderer::VulkanDevice* device = nullptr;
    renderer::GpuAllocator* allocator = nullptr;
    renderer::BindlessDescriptors* bindless = nullptr; ///< required (the path tracer's pipeline layout)
    PtKernelLanguage language = PtKernelLanguage::Auto;
    u32 framesInFlight = 3;
};

struct RadianceCacheGraphRefs {
    renderer::rg::BufferRef table;
    renderer::rg::BufferRef records;
    renderer::rg::BufferRef probe; ///< valid with enableProbe
    renderer::rg::BufferRef ring;
    renderer::rg::BufferRange ringRange{};
    bool valid = false;
};

struct RadianceCacheGpuStats {
    u32 frames = 0;
    u32 passes = 0; ///< this frame
    u32 clears = 0;
    u32 reallocations = 0;
};

class RadianceCacheGpu {
public:
    RadianceCacheGpu();
    ~RadianceCacheGpu();
    RadianceCacheGpu(const RadianceCacheGpu&) = delete;
    RadianceCacheGpu& operator=(const RadianceCacheGpu&) = delete;

    bool init(const RadianceCacheGpuDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// This frame's stage block and buffers (after PathTracerGpu::beginFrame of the same frame). The frame index of
    /// the params (training pixels) counts beginFrame calls, as RadianceCacheCpu::train does.
    bool beginFrame(u64 frameSerial, const PtCompiledScene& scene, const PtFrameDesc& frame,
                    const RadianceCacheSettings& settings);
    /// The passes above, and PathTracerGpu::setRadianceCache for its trace pass (add it afterwards).
    bool addPasses(renderer::rg::Graph& graph, PathTracerGpu& pt, const PtGraphRefs& ptRefs);
    const RadianceCacheGraphRefs& graphRefs() const { return m_refs; }
    u32 collectRetired(u64 completedSerial);
    /// The next frame clears the table first and the frame index (training pixels) starts over.
    void reset() {
        m_needsClear = true;
        m_frameIndex = 0;
    }
    /// Gates: after the resolve, "relight.radiance_cache.probe" writes, per training record, the resolved radiance of
    /// its cell and the sample count (w = -1: invalid record or no answer) - the GPU query on read-back inputs.
    void enableProbe(bool on) { m_probeEnabled = on; }

    /// Read-back (after the frame completed): the table (u32 words, header first), the records (float4 words), the
    /// frame's packed params (kRcParamWords), the capacity and the record count.
    const u32* mappedTable() const;
    const Word* mappedRecords() const;
    const Word* mappedProbe() const;
    const Word* params() const { return m_params; }
    u32 capacity() const { return m_capacity; }
    u32 recordCount() const { return m_tiles * kRcMaxVertices; }
    u64 tableAddress() const { return m_table.deviceAddress + m_tableOffset; } ///< 256-byte aligned
    const RadianceCacheGpuStats& stats() const { return m_stats; }

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
        u32 width = 0;
        u32 height = 0;
        u32 lightCount = 0;
        u32 stage = 0;
        u32 count = 0;
        u32 pitch = 0;
    };
    static_assert(sizeof(Push) == 120u, "RcPush (radiance_cache.comp / .slang)");
    struct PassRecord {
        RadianceCacheGpu* self = nullptr;
        Push push{};
        u32 groupsX = 1;
        u32 groupsY = 1;
    };
    struct Retired {
        renderer::Buffer buffer{};
        u64 serial = 0;
    };
    static constexpr u32 kMaxPasses = 5u;
    static constexpr u32 kMaxSlots = 8u;

    bool createPipeline();
    bool ensureBuffer(renderer::Buffer& buffer, u64 bytes, u32 memory, const char* name, u8* queue);
    static void record(const renderer::rg::PassContext& context, void* user);

    RadianceCacheGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    renderer::Buffer m_table{};
    u64 m_tableOffset = 0; ///< the table's first byte in m_table (256-byte aligned device address)
    u8 m_tableQueue = 0xFFu;
    renderer::Buffer m_records{};
    u8 m_recordsQueue = 0xFFu;
    renderer::Buffer m_probe{};
    u8 m_probeQueue = 0xFFu;
    bool m_probeEnabled = false;
    renderer::Buffer m_ring{};
    u8 m_ringQueue = 0xFFu;
    u64 m_blockOffset = 0;
    u64 m_tableBytes = 0;
    u64 m_recordBytes = 0;
    std::vector<Retired> m_retired;
    PassRecord m_passes[kMaxPasses]{};
    RadianceCacheGraphRefs m_refs{};
    Word m_params[kRcParamWords] = {};
    u64 m_frameSerial = 0;
    u32 m_frameIndex = 0;
    u32 m_capacity = 0;
    u32 m_tiles = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_needsClear = true;
    bool m_frameReady = false;
    void* m_layoutHandle = nullptr;
    void* m_pipeline = nullptr;
    RadianceCacheGpuStats m_stats{};
};

} // namespace fuse::relight::render::pathtrace
