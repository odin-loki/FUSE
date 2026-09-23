#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/world_partition/cell_file.hpp>
#include <fuse/world_partition/grid_cell.hpp>
#include <fuse/world_partition/residency_set.hpp>
#include <fuse/world_partition/streaming_budget.hpp>
#include <fuse/world_partition/streaming_request_queue.hpp>
#include <fuse/world_partition/streaming_volume.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::ecs {
class Registry;
} // namespace fuse::ecs

namespace fuse::world_partition {

struct CellStagingStore;

struct WorldPartitionDesc {
    f32 cell_size = 256.f;
    f32 stream_in_distance = 512.f;
    f32 stream_out_distance = 600.f;
    u32 max_loaded_cells = 64;
    u64 default_cell_bytes = 1u << 20; ///< Stub per-cell resident footprint (1 MiB)
    StreamingBudget budget{};
    EvictionPolicy eviction_policy = EvictionPolicy::DistanceFromFocus;
    bool async_loading = true;
    /// Directory holding `cells/cell_<x>_<y>.fusecell` files. Empty = no disk I/O (stub cells).
    /// Worker threads read and decode cell files; entities are spawned on the game thread.
    std::string cell_root;
};

/// Load/unload callback hooks — production wiring supplies scene + asset I/O.
struct CellLoadCallbacks {
    void (*on_load)(WorldCell& cell) = nullptr;
    void (*on_unload)(WorldCell& cell) = nullptr;
};

/// World partition grid with streaming residency stubs (B7.6).
class WorldPartition {
public:
    WorldPartition() = default;

    void init(const WorldPartitionDesc& desc);
    void destroy();

    void set_callbacks(CellLoadCallbacks callbacks) { m_callbacks = callbacks; }

    /// Registry that receives entities from loaded cell files (non-owning; must outlive the
    /// partition or be detached with nullptr). Unloading a cell destroys its entities.
    void set_registry(fuse::ecs::Registry* registry) { m_registry = registry; }

    /// Re-evaluate streaming volume and advance load/unload queues.
    void update(fuse::ecs::vec3 camera_pos);

    /// Load a cell synchronously on the calling thread (teleport path). On return the cell is Resident
    /// with its entities spawned, unless the budget rejected it or its file is corrupt (returns false).
    bool force_load(GridCoord coord);
    void force_unload(GridCoord coord);

    [[nodiscard]] bool cell_loaded(GridCoord coord) const;
    [[nodiscard]] CellResidencyState cell_residency(GridCoord coord) const;
    [[nodiscard]] u32 loaded_cell_count() const;
    [[nodiscard]] u32 resident_cell_count() const;
    [[nodiscard]] u64 resident_byte_count() const;
    [[nodiscard]] const ResidencySet& residency_set() const { return m_residency_set; }
    [[nodiscard]] u32 queued_load_count() const;
    [[nodiscard]] u32 queued_unload_count() const;
    [[nodiscard]] u32 rejected_load_count() const;
    /// Loads that failed because the cell file was corrupt.
    [[nodiscard]] u32 failed_load_count() const { return m_failed_loads; }
    /// Resident plus in-progress load footprint used for budget admission.
    [[nodiscard]] u32 committed_cell_count() const;
    [[nodiscard]] u64 committed_byte_count() const;
    [[nodiscard]] const StreamingBudgetCounters& budget_counters() const { return m_budget_counters; }
    [[nodiscard]] u32 in_flight_request_count() const;
    [[nodiscard]] u32 pending_completion_count() const;
    /// True when no load/unload is queued, buffered for submission, in flight or awaiting drain.
    [[nodiscard]] bool streaming_idle() const;
    [[nodiscard]] u32 current_tick() const { return m_tick; }
    [[nodiscard]] const WorldCell* find_cell(GridCoord coord) const;
    [[nodiscard]] const WorldPartitionDesc& desc() const { return m_desc; }

    /// Apply JobScheduler completions queued since the last drain (also called from update).
    u32 drain_completed_requests();

private:
    struct LoadRequest {
        GridCoord coord{};
        f32 priority = 0.f;
    };

    struct UnloadRequest {
        GridCoord coord{};
        f32 priority = 0.f;
    };

    WorldCell& ensure_cell_(GridCoord coord);
    [[nodiscard]] const WorldCell* find_cell_(GridCoord coord) const;
    [[nodiscard]] bool queue_load_(GridCoord coord, f32 priority);
    void queue_unload_(GridCoord coord, f32 priority);
    void process_queues_();
    void evict_for_budget_(f32 incoming_priority, u64 incoming_bytes);
    [[nodiscard]] bool can_accept_load_(u64 incoming_bytes) const;
    [[nodiscard]] WorldCell* find_budget_eviction_candidate_(f32 incoming_priority, f32& out_score);
    void touch_cell_(WorldCell& cell);
    [[nodiscard]] f32 eviction_score_for_(const WorldCell& cell) const;
    [[nodiscard]] f32 budget_eviction_score_for_(const WorldCell& cell) const;
    void drain_completed_requests_();
    bool execute_load_(WorldCell& cell);
    [[nodiscard]] bool take_staged_(GridCoord coord, CellFileData& out, u64& bytes);
    void spawn_cell_entities_(WorldCell& cell, const CellFileData& data);
    void destroy_cell_entities_(WorldCell& cell);
    [[nodiscard]] u64 incoming_bytes_for_(const WorldCell& cell) const;
    void execute_unload_(WorldCell& cell);
    void apply_completed_request_(const CompletedStreamingRequest& completed);
    void collect_stream_candidates_(fuse::ecs::vec3 camera_pos);
    [[nodiscard]] bool use_async_jobs_() const;
    [[nodiscard]] StreamingWorkFn make_worker_stub_() const;
    void flush_async_queue_(u32 max_submits);

    WorldPartitionDesc m_desc{};
    CellLoadCallbacks m_callbacks{};
    u32 m_tick = 0;
    StreamingBudgetCounters m_budget_counters{};
    std::unordered_map<u64, WorldCell> m_cells;
    std::vector<LoadRequest> m_load_queue;
    std::vector<UnloadRequest> m_unload_queue;
    StreamingVolume m_streaming{};
    StreamingRequestQueue m_async_queue{};
    ResidencySet m_residency_set{};
    std::vector<CompletedStreamingRequest> m_completed_batch_;
    std::vector<LoadRequest> m_candidates_;
    std::shared_ptr<CellStagingStore> m_staging;
    fuse::ecs::Registry* m_registry = nullptr;
    u32 m_failed_loads = 0;
};

} // namespace fuse::world_partition
