#pragma once

// WP-5.3 cluster streaming on the GPU (renderer plan Phase 5 "cluster page streaming and residency"):
// the residency-aware LOD cut + streaming feedback as a render-graph v2 compute pass, page uploads into a
// pool of fixed-size slots through the UploadQueue, and the feedback read back to drive the CPU residency
// model (ClusterStreamer / core_logic ClusterPageResidency).
//
// Per frame:
//   stream.beginFrame(serial, view, completedSerial);   // finished uploads -> resident; newest completed
//                                                        // frame's feedback -> requests; residency update
//                                                        // (evictions, loads staged into the UploadQueue)
//   upload.flush();                                      // the caller's (page copies ride with other uploads)
//   StreamGraphRefs r = stream.importInto(graph);
//   stream.addFrame(graph, r);
//     "stream.state"     resident-page bitmask + page -> slot table (vkCmdUpdateBuffer, this frame's copy),
//                        feedback reset (vkCmdFillBuffer)
//     "stream.cut"       one thread per cluster: stream_cut_kernel.hpp (geometry_stream_cut) -> cut flags,
//                        draw list, feedback (per-page priorities + request list)
//     "stream.feedback"  feedback -> host-visible ring slot of this frame (read framesInFlight later, or as
//                        soon as completedSerial says the frame finished)
//
// Outputs for a page-aware draw path (WP-5.1 hook): drawListBuffer() (cluster ids, count = feedback word
// kFeedbackDrawCount), stateBuffer() (bits, then the u32 page -> slot table at stateSlotTableOffset()),
// poolBuffer() / poolAddress() (slot s at s * pageBytes(); payload layout in cluster_page_types.hpp).
//
// Coarse guarantee: init() uploads the coarse pages and waits for them, so the first frame already has the
// root / terminal LOD; the residency model never evicts them. Budget: StreamerDesc::budget_pages slots,
// never exceeded (pool = budget + reserve slots; see cluster_streamer.hpp for slot reuse).
// Steady-state frames make no heap allocations (fixed pass records, preallocated streamer state).
// Stub backend / no device: init() fails; the CPU pieces (page file, streamer, kernel) remain available.

#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry_streaming/cluster_page_file.hpp>
#include <fuse/renderer/geometry_streaming/cluster_streamer.hpp>
#include <fuse/renderer/geometry_streaming/stream_cut_kernel.hpp>
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

namespace fuse::renderer::geometry_streaming {

enum class StreamKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct StreamingCapabilities {
    bool ok = false;
    const char* reason = "no device"; ///< "ok" when usable
};
StreamingCapabilities queryStreamingCapabilities(const VulkanDevice* device);

/// Push constants of "stream.cut" (64 bytes; stream_cut.{comp,slang}).
struct StreamCutPush {
    f32 camera[3] = {0.f, 0.f, 0.f};
    f32 errorScale = 1.f;
    f32 threshold = 1.f;
    f32 znear = 0.01f;
    u32 clusterCount = 0;
    u32 pageCount = 0;
    u32 links = 0;    ///< bindless handles (storage buffers, u32 words)
    u32 info = 0;
    u32 state = 0;
    u32 feedback = 0;
    u32 cut = 0;
    u32 drawList = 0;
    u32 pad[2] = {0u, 0u};
};
static_assert(sizeof(StreamCutPush) == 64u, "StreamCutPush layout");

struct GeometryStreamingDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    UploadQueue* upload = nullptr;                  ///< page and table uploads (must outlive this object)
    const geometry::dag::ClusterDag* dag = nullptr; ///< links (must outlive init())
    const ClusterPageFile* file = nullptr;          ///< page table + payloads (must outlive this object)
    StreamerDesc streamer{};
    u32 framesInFlight = 3;                         ///< feedback ring slots
    StreamKernelLanguage language = StreamKernelLanguage::Auto;
    const char* name = "stream";
};

struct StreamGraphRefs {
    rg::BufferRef links;
    rg::BufferRef info;
    rg::BufferRef state;
    rg::BufferRef feedback;
    rg::BufferRef cut;
    rg::BufferRef drawList;
    rg::BufferRef pool;
    rg::BufferRef readback;
};

struct StreamGpuStats {
    u32 uploadsIssued = 0;    ///< this frame
    u32 uploadsCompleted = 0; ///< this frame
    u32 uploadFailures = 0;   ///< staging failures this frame (loads failed, retried later)
    u32 feedbackFrames = 0;   ///< feedback frames consumed so far
    u64 feedbackSerial = 0;   ///< serial whose feedback this frame applied (0: none)
    u32 passes = 0;           ///< stream.* passes added this frame
};

class GeometryStreaming {
public:
    GeometryStreaming() = default;
    ~GeometryStreaming();
    GeometryStreaming(const GeometryStreaming&) = delete;
    GeometryStreaming& operator=(const GeometryStreaming&) = delete;

    /// Creates the buffers, uploads links / cluster info / coarse pages and waits for them. Fails without a
    /// capable device, a built kernel of the requested language, a valid file / DAG pair, or in the stub.
    bool init(const GeometryStreamingDesc& desc);
    /// The caller must have retired every frame that used the resources.
    void destroy();
    bool valid() const { return m_initialized; }

    /// `serial` increases by >= 1 per frame (> every serial before); frames <= completedSerial finished.
    bool beginFrame(u64 serial, const geometry::dag::cut_kernel::DagView& view, u64 completedSerial);
    StreamGraphRefs importInto(rg::Graph& graph);
    void addFrame(rg::Graph& graph, const StreamGraphRefs& refs);

    // --- inspection / hooks -------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const ClusterStreamer& streamer() const { return m_streamer; }
    ClusterStreamer& streamer() { return m_streamer; }
    /// The resident bitmask this frame's cut uses (host copy uploaded by stream.state).
    const u32* frameResidentBits() const { return m_stateHost.data(); }
    u32 frameResidentWords() const { return m_bitWords; }
    const geometry::dag::cut_kernel::DagView& frameView() const { return m_view; }
    /// Feedback words of frame `serial` in the read-back ring (valid once that frame completed and until
    /// its ring slot is reused framesInFlight frames later); nullptr when not recorded.
    const u32* feedbackHost(u64 serial) const;
    u32 feedbackWords() const { return m_layout.words; }
    const Buffer& cutBuffer() const { return m_cut.buffer; }
    const Buffer& drawListBuffer() const { return m_drawList.buffer; }
    const Buffer& feedbackBuffer() const { return m_feedback.buffer; }
    const Buffer& stateBuffer() const { return m_state.buffer; }
    const Buffer& poolBuffer() const { return m_pool.buffer; }
    u64 poolAddress() const { return m_pool.buffer.deviceAddress; }
    u64 stateSlotTableOffset() const { return static_cast<u64>(m_bitWords) * 4u; }
    u32 pageBytes() const { return m_desc.file != nullptr ? m_desc.file->page_bytes : 0u; }
    u32 clusterCount() const { return m_clusterCount; }
    const StreamGpuStats& stats() const { return m_stats; }

private:
    struct OwnedBuffer {
        Buffer buffer{};
        BindlessSlotHandle slot{};
        u32 handle = 0;
        u8 queue = rg::kNoQueue;
    };
    struct PendingUpload {
        u32 page = 0;
        u64 ticketSerial = 0;
    };
    enum RecordKind : u8 { kState = 0, kCut, kFeedbackCopy };
    struct PassRecord {
        GeometryStreaming* self = nullptr;
        RecordKind kind = kState;
    };
    static constexpr u32 kMaxPasses = 4u;

    bool createPipeline();
    bool createBuffer(OwnedBuffer& out, u64 bytes, bool bindless, MemoryUsage memory, const char* name);
    void destroyBuffer(OwnedBuffer& b);
    bool uploadBlob(OwnedBuffer& dst, const void* data, u64 bytes);
    bool issueLoads();
    void pollUploads();
    static void recordPass(const rg::PassContext& context, void* user);

    GeometryStreamingDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    ClusterStreamer m_streamer;
    stream_kernel::FeedbackLayout m_layout{};
    u32 m_clusterCount = 0;
    u32 m_pageCount = 0;
    u32 m_bitWords = 0;
    u32 m_frameOffset = 0;      ///< residency frame = serial + offset (init's coarse frames come first)
    u64 m_serial = 0;
    u64 m_lastConsumed = 0;
    u64 m_lastRecorded = 0;
    OwnedBuffer m_links{};
    OwnedBuffer m_info{};
    OwnedBuffer m_state{};
    OwnedBuffer m_feedback{};
    OwnedBuffer m_cut{};
    OwnedBuffer m_drawList{};
    OwnedBuffer m_pool{};
    OwnedBuffer m_readback{};
    std::vector<u64> m_ringSerial;   ///< serial recorded into each ring slot
    std::vector<u32> m_stateHost;    ///< bits then slot table, this frame
    std::vector<PendingUpload> m_pending;
    u32 m_pendingCount = 0;
    geometry::dag::cut_kernel::DagView m_view{};
    StreamCutPush m_push{};
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr;  ///< VkPipelineLayout (bindless set + 64-byte push, compute)
    void* m_pipeline = nullptr;
    StreamGpuStats m_stats{};
};

} // namespace fuse::renderer::geometry_streaming
