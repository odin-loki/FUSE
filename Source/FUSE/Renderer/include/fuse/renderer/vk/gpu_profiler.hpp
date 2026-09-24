#pragma once

// WP-0.6 GPU profiler (docs/unification/RENDERER-EXECUTION.md): GPU zones from Vulkan timestamp
// queries, one zone per render-graph pass through rg::PassHooks, plus TracyVk zones when the build
// has the Tracy backend (FUSE_TRACY=ON, <fuse/profiler/tracy_adapter.hpp>).
//
//   auto gpu = GpuProfiler::create(device);
//   executor->setPassHooks(gpu->passHooks());   // WP-0.3 hook: a zone around every executed pass
//   for (;;) {
//       gpu->beginFrame();
//       executor->execute(graph);                // or beginZone/endZone around any recording
//       gpu->endFrame();                         // + Tracy frame mark (FUSE_PROFILE_FRAME_MARK)
//       gpu->resolve();                          // non-blocking; latest() = newest complete frame
//   }
//
// Timestamp path (always, and the only path without Tracy): one TIMESTAMP query pool, a
// `maxZonesPerFrame * 2` query range per frame slot, TOP_OF_PIPE / BOTTOM_OF_PIPE writes, each
// zone resetting its own query pair in the recording command buffer (no host query reset needed).
// A frame slot is reused `framesInFlight` frames later; keep it above the executor's frames in
// flight so its queries have retired (an unfinished frame is dropped, never waited on). Zones on a
// queue family without timestamp support (timestampValidBits == 0) are skipped and counted.
// TracyVk path (FUSE_TRACY builds): one TracyVkCtx per distinct queue, VkCtxScope per zone,
// TracyVkCollect on the first zone of each queue per frame; with TRACY_ON_DEMAND it is inert until
// a server connects.
// Zones must be begun and ended outside render pass instances (rg passes are) and nest LIFO.
// Stub backend: create() returns an invalid profiler whose calls are no-ops.

#include <fuse/renderer/rg/executor.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

class VulkanDevice;

struct GpuProfilerDesc {
    /// Frame slots of the query ring (>= rg::ExecutorDesc::framesInFlight + 1).
    u32 framesInFlight = 3;
    u32 maxZonesPerFrame = 256;
    /// Create TracyVk contexts (only in FUSE_TRACY builds; ignored otherwise).
    bool enableTracy = true;
    /// Prefix of the Vulkan object names (VK_EXT_debug_utils) and Tracy context names.
    const char* name = "fuse.gpu";
};

struct GpuZoneTiming {
    static constexpr u32 kNameCapacity = 64;
    char name[kNameCapacity] = {};
    rg::QueueClass queue = rg::QueueClass::Graphics;
    u32 depth = 0;
    /// Device time in nanoseconds (ticks * timestampPeriod), masked to timestampValidBits.
    u64 beginNs = 0;
    u64 endNs = 0;
    u64 durationNs() const { return endNs >= beginNs ? endNs - beginNs : 0; }
};

struct GpuFrameTimings {
    u64 frame = 0;
    bool valid = false;
    std::vector<GpuZoneTiming> zones;
    /// Zones not recorded that frame: ring full (maxZonesPerFrame), no timestamp support on the
    /// queue, or unbalanced begin/end.
    u32 skippedZones = 0;
};

struct GpuProfilerStats {
    u64 framesBegun = 0;
    u64 framesResolved = 0;
    u64 framesDropped = 0; ///< slot reused before its queries were available
    u64 zonesRecorded = 0;
    u64 zonesSkipped = 0;
    u64 tracyCollects = 0;
};

class GpuProfiler {
public:
    static constexpr u32 kInvalidZone = UINT32_MAX;

    static std::unique_ptr<GpuProfiler> create(VulkanDevice& device, const GpuProfilerDesc& desc = {});
    ~GpuProfiler();

    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    bool isValid() const { return m_valid; }
    const std::string& message() const { return m_message; }
    /// Timestamp support per rg::QueueClass (the family the device maps it to).
    bool timestampsSupported(rg::QueueClass queue) const;
    /// Number of TracyVk contexts (0 without the Tracy backend or with enableTracy = false).
    u32 tracyContextCount() const;
    /// VkQueryPool of the timestamp ring (named "<name>.timestamps").
    void* queryPool() const;

    void beginFrame();
    void endFrame();

    /// Record a begin timestamp into `commandBuffer` (VkCommandBuffer submitted on `queue`).
    /// Returns kInvalidZone when the zone is skipped (endZone accepts it).
    u32 beginZone(void* commandBuffer, const char* name, rg::QueueClass queue = rg::QueueClass::Graphics);
    void endZone(void* commandBuffer, u32 zone);

    /// Hooks for rg::Executor::setPassHooks: one zone per executed pass, named after the pass.
    rg::PassHooks passHooks();

    /// Non-blocking. Resolves ended frames, oldest first, while all their queries are available.
    /// Returns the number of frames resolved by this call.
    u32 resolve();
    /// The most recently resolved frame (valid == false before the first).
    const GpuFrameTimings& latest() const { return m_latest; }
    const GpuProfilerStats& stats() const { return m_stats; }

private:
    GpuProfiler() = default;
    bool initialize(VulkanDevice& device, const GpuProfilerDesc& desc);
    void shutdown();
    bool resolveSlot(u32 slot, bool publish);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    GpuFrameTimings m_latest;
    GpuProfilerStats m_stats;
    std::string m_message;
    bool m_valid = false;
};

} // namespace fuse::renderer
