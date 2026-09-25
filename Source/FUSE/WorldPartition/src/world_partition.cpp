#include <fuse/world_partition/world_partition.hpp>

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/platform/sleep.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>

namespace fuse::world_partition {

/// Decoded cell files handed from worker threads to the game thread. Shared with in-flight jobs so
/// a job never touches the partition itself.
struct CellStagingStore {
    struct Staged {
        CellFileData data;
        u64 bytes = 0;
    };

    std::mutex mutex;
    std::unordered_map<u64, Staged> cells;

    void put(GridCoord coord, CellFileData&& data, u64 bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        Staged& staged = cells[grid_coord_key(coord)];
        staged.data = std::move(data);
        staged.bytes = bytes;
    }

    bool take(GridCoord coord, CellFileData& out, u64& bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = cells.find(grid_coord_key(coord));
        if (it == cells.end()) {
            return false;
        }
        out = std::move(it->second.data);
        bytes = it->second.bytes;
        cells.erase(it);
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        cells.clear();
    }
};

namespace {

std::string cell_file_path(const std::string& root, GridCoord coord) {
    return root + "/" + cell_asset_relative_path(coord);
}

} // namespace

void WorldPartition::init(const WorldPartitionDesc& desc) {
    destroy();
    m_desc = desc;
    m_streaming.desc.stream_in_radius = desc.stream_in_distance;
    m_streaming.desc.stream_out_radius = desc.stream_out_distance;
    m_async_queue.set_max_pending_submits(desc.budget.max_async_in_flight);
    m_staging = std::make_shared<CellStagingStore>();
}

void WorldPartition::destroy() {
    // Let in-flight jobs retire so stale completions cannot leak into a re-initialised partition.
    // Up to ~1 s: sleepAtLeast, because sleep_for(200us) returns at once on MinGW.
    for (int attempt = 0; attempt < 5000 && m_async_queue.in_flight_count() > 0u; ++attempt) {
        fuse::platform::sleepAtLeast(std::chrono::microseconds(200));
    }
    for (auto& entry : m_cells) {
        WorldCell& cell = entry.second;
        if (is_resident_state(cell.residency) || is_transitional_state(cell.residency)) {
            execute_unload_(cell);
        }
    }
    m_cells.clear();
    m_load_queue.clear();
    m_unload_queue.clear();
    m_completed_batch_.clear();
    m_async_queue.clear();
    m_residency_set.clear();
    if (m_staging) {
        m_staging->clear();
    }
    m_callbacks = {};
    m_tick = 0;
    m_budget_counters = {};
    m_failed_loads = 0;
}

void WorldPartition::update(fuse::ecs::vec3 camera_pos) {
    ++m_tick;
    drain_completed_requests_();
    m_streaming.center = camera_pos;
    collect_stream_candidates_(camera_pos);
    process_queues_();
}

bool WorldPartition::force_load(GridCoord coord) {
    if (!queue_load_(coord, std::numeric_limits<f32>::max())) {
        process_queues_();
        return false;
    }

    WorldCell& cell = ensure_cell_(coord);
    bool loaded = is_resident_state(cell.residency);
    if (!loaded) {
        // Pull the request out of the async pipeline and complete it here; any job already in
        // flight for this cell finds it Resident on completion and is discarded.
        m_load_queue.erase(std::remove_if(m_load_queue.begin(), m_load_queue.end(),
                                          [&](const LoadRequest& request) { return request.coord == coord; }),
                           m_load_queue.end());
        cell.residency = CellResidencyState::Loading;
        loaded = execute_load_(cell);
    }
    process_queues_();
    return loaded;
}

void WorldPartition::force_unload(GridCoord coord) {
    queue_unload_(coord, std::numeric_limits<f32>::max());
    process_queues_();
}

bool WorldPartition::cell_loaded(GridCoord coord) const {
    const WorldCell* cell = find_cell_(coord);
    return cell != nullptr && is_resident_state(cell->residency);
}

CellResidencyState WorldPartition::cell_residency(GridCoord coord) const {
    const WorldCell* cell = find_cell_(coord);
    return cell != nullptr ? cell->residency : CellResidencyState::Unloaded;
}

u32 WorldPartition::loaded_cell_count() const {
    u32 count = 0;
    for (const auto& entry : m_cells) {
        if (is_resident_state(entry.second.residency)) {
            ++count;
        }
    }
    return count;
}

u32 WorldPartition::resident_cell_count() const { return loaded_cell_count(); }

u32 WorldPartition::committed_cell_count() const {
    u32 count = 0;
    for (const auto& entry : m_cells) {
        const CellResidencyState state = entry.second.residency;
        if (is_resident_state(state) || is_loading_state(state)) {
            ++count;
        }
    }
    return count;
}

u64 WorldPartition::committed_byte_count() const {
    u64 bytes = 0;
    for (const auto& entry : m_cells) {
        const WorldCell& cell = entry.second;
        if (is_resident_state(cell.residency)) {
            bytes += cell.resident_bytes;
        } else if (is_loading_state(cell.residency)) {
            bytes += incoming_bytes_for_(cell);
        }
    }
    return bytes;
}

u64 WorldPartition::incoming_bytes_for_(const WorldCell& cell) const {
    return cell.resident_bytes > 0u ? cell.resident_bytes : m_desc.default_cell_bytes;
}

u64 WorldPartition::resident_byte_count() const {
    u64 bytes = 0;
    for (const auto& entry : m_cells) {
        if (is_resident_state(entry.second.residency)) {
            bytes += entry.second.resident_bytes;
        }
    }
    return bytes;
}

u32 WorldPartition::queued_load_count() const { return static_cast<u32>(m_load_queue.size()); }

u32 WorldPartition::rejected_load_count() const { return m_budget_counters.rejected_loads; }

u32 WorldPartition::queued_unload_count() const { return static_cast<u32>(m_unload_queue.size()); }

u32 WorldPartition::in_flight_request_count() const { return m_async_queue.in_flight_count(); }

u32 WorldPartition::pending_completion_count() const { return m_async_queue.completed_count(); }

bool WorldPartition::streaming_idle() const {
    return m_load_queue.empty() && m_unload_queue.empty() && m_async_queue.empty();
}

const WorldCell* WorldPartition::find_cell(GridCoord coord) const { return find_cell_(coord); }

u32 WorldPartition::drain_completed_requests() {
    drain_completed_requests_();
    return static_cast<u32>(m_completed_batch_.size());
}

WorldCell& WorldPartition::ensure_cell_(GridCoord coord) {
    const u64 key = grid_coord_key(coord);
    auto it = m_cells.find(key);
    if (it != m_cells.end()) {
        return it->second;
    }

    WorldCell cell{};
    cell.coord = coord;
    cell.world_bounds = grid_cell_bounds(coord, m_desc.cell_size);
    cell.asset_path = cell_asset_relative_path(coord);
    const auto inserted = m_cells.emplace(key, std::move(cell));
    return inserted.first->second;
}

const WorldCell* WorldPartition::find_cell_(GridCoord coord) const {
    const auto it = m_cells.find(grid_coord_key(coord));
    return it != m_cells.end() ? &it->second : nullptr;
}

bool WorldPartition::can_accept_load_(u64 incoming_bytes) const {
    // Count in-progress loads too: async completions must never push residency past the budget.
    if (!can_accept_resident_cell(m_desc.max_loaded_cells, committed_cell_count())) {
        return false;
    }
    return !would_exceed_byte_budget(m_desc.budget.max_resident_bytes, committed_byte_count(), incoming_bytes);
}

void WorldPartition::touch_cell_(WorldCell& cell) {
    cell.last_touch_tick = m_tick;
}

f32 WorldPartition::eviction_score_for_(const WorldCell& cell) const {
    const f32 distance_priority =
        m_streaming.unload_priority_for(cell.coord, m_desc.cell_size);
    return eviction_score_for_guarded(distance_priority, cell.last_touch_tick, m_tick, m_desc.eviction_policy);
}

f32 WorldPartition::budget_eviction_score_for_(const WorldCell& cell) const {
    const f32 focus_distance = m_residency_set.focus_distance_for(cell.coord);
    const f32 distance_priority =
        m_streaming.unload_priority_for(cell.coord, m_desc.cell_size);
    return budget_eviction_score_for_guarded(focus_distance, distance_priority, cell.last_touch_tick, m_tick,
                                             m_desc.eviction_policy);
}

WorldCell* WorldPartition::find_budget_eviction_candidate_(f32 incoming_priority, f32& out_score) {
    out_score = -1.f;

    if (m_desc.eviction_policy == EvictionPolicy::DistanceFromFocus) {
        const GridCoord picked = pick_budget_eviction_candidate_from_set(
            m_residency_set,
            [&](GridCoord coord) {
                // Only Resident cells can be evicted; ones already queued/unloading free nothing more.
                const WorldCell* cell = find_cell_(coord);
                return cell != nullptr && is_resident_state(cell->residency) ? budget_eviction_score_for_(*cell)
                                                                             : -1.f;
            },
            incoming_priority, m_desc.eviction_policy, out_score);

        if (!is_valid_grid_coord(picked) || !is_positive_eviction_score(out_score)) {
            return nullptr;
        }
        // Never trade a resident for an incoming cell that is no closer to the focus: that swap
        // frees nothing useful and makes the pair ping-pong every tick.
        if (incoming_priority < std::numeric_limits<f32>::max()) {
            const f32 incoming_distance = m_streaming.desc.stream_in_radius - incoming_priority;
            if (incoming_distance >= m_residency_set.focus_distance_for(picked)) {
                out_score = -1.f;
                return nullptr;
            }
        }
        return const_cast<WorldCell*>(find_cell_(picked));
    }

    WorldCell* best_candidate = nullptr;
    for (auto& entry : m_cells) {
        WorldCell& cell = entry.second;
        if (!is_resident_state(cell.residency)) {
            continue;
        }

        const f32 score = budget_eviction_score_for_(cell);
        if (!can_evict_for_incoming(incoming_priority, score, m_desc.eviction_policy)) {
            continue;
        }
        if (score > out_score) {
            out_score = score;
            best_candidate = &cell;
        }
    }
    return best_candidate;
}

void WorldPartition::evict_for_budget_(f32 incoming_priority, u64 incoming_bytes) {
    if (!can_attempt_budget_eviction_from_set(m_residency_set, m_desc.max_loaded_cells, committed_cell_count(),
                                              m_desc.budget.max_resident_bytes, committed_byte_count(),
                                              incoming_bytes)) {
        if (needs_budget_eviction_for_incoming(m_desc.max_loaded_cells, committed_cell_count(),
                                               m_desc.budget.max_resident_bytes, committed_byte_count(),
                                               incoming_bytes) &&
            !m_residency_set.has_eviction_candidate()) {
            ++m_budget_counters.eviction_skipped;
        }
        return;
    }

    while (needs_budget_eviction_for_incoming(m_desc.max_loaded_cells, committed_cell_count(),
                                              m_desc.budget.max_resident_bytes, committed_byte_count(),
                                              incoming_bytes)) {
        f32 best_score = -1.f;
        WorldCell* best_candidate = find_budget_eviction_candidate_(incoming_priority, best_score);

        if (best_candidate == nullptr || !is_positive_eviction_score(best_score) ||
            !is_resident_state(best_candidate->residency)) {
            ++m_budget_counters.eviction_skipped;
            break;
        }

        const f32 focus_distance = m_residency_set.focus_distance_for(best_candidate->coord);
        const f32 streaming_priority =
            m_streaming.unload_priority_for(best_candidate->coord, m_desc.cell_size);
        const f32 unload_priority = eviction_unload_priority_guarded(
            streaming_priority, best_candidate->unload_priority, focus_distance, streaming_priority,
            best_candidate->last_touch_tick, m_tick, m_desc.eviction_policy);

        m_budget_counters.budget_evictions += 1u;
        m_budget_counters.bytes_evicted += best_candidate->resident_bytes;
        queue_unload_(best_candidate->coord, unload_priority);
    }

    process_queues_();
}

bool WorldPartition::queue_load_(GridCoord coord, f32 priority) {
    WorldCell& cell = ensure_cell_(coord);
    if (is_resident_state(cell.residency) || is_loading_state(cell.residency)) {
        cell.load_priority = std::max(cell.load_priority, priority);
        touch_cell_(cell);
        return true;
    }

    if (is_unloading_state(cell.residency)) {
        // Cancel the pending unload: the cell's content (and entities) are still in place, and any
        // unload job already in flight is discarded on completion because the cell is Resident.
        // The cell re-enters the committed set, so it passes budget admission like a fresh load.
        const u64 reclaim_bytes = incoming_bytes_for_(cell);
        if (needs_budget_eviction_for_incoming(m_desc.max_loaded_cells, committed_cell_count(),
                                               m_desc.budget.max_resident_bytes, committed_byte_count(),
                                               reclaim_bytes)) {
            evict_for_budget_(priority, reclaim_bytes);
        }
        if (!can_accept_load_(reclaim_bytes)) {
            ++m_budget_counters.rejected_loads;
            return false;
        }
        m_unload_queue.erase(std::remove_if(m_unload_queue.begin(), m_unload_queue.end(),
                                            [&](const UnloadRequest& request) { return request.coord == coord; }),
                             m_unload_queue.end());
        cell.residency = CellResidencyState::Resident;
        cell.visible = true;
        cell.unload_priority = 0.f;
        cell.load_priority = std::max(cell.load_priority, priority);
        touch_cell_(cell);
        return true;
    }

    const u64 incoming_bytes = incoming_bytes_for_(cell);
    if (needs_budget_eviction_for_incoming(m_desc.max_loaded_cells, committed_cell_count(),
                                           m_desc.budget.max_resident_bytes, committed_byte_count(),
                                           incoming_bytes)) {
        evict_for_budget_(priority, incoming_bytes);
    }
    if (!can_accept_load_(incoming_bytes)) {
        ++m_budget_counters.rejected_loads;
        return false;
    }

    cell.residency = CellResidencyState::QueuedLoad;
    cell.load_priority = priority;

    const auto already_queued = std::find_if(m_load_queue.begin(), m_load_queue.end(),
                                             [&](const LoadRequest& request) {
                                                 return request.coord == coord;
                                             });
    if (already_queued != m_load_queue.end()) {
        already_queued->priority = std::max(already_queued->priority, priority);
        return true;
    }

    m_load_queue.push_back({coord, priority});
    return true;
}

void WorldPartition::queue_unload_(GridCoord coord, f32 priority) {
    WorldCell* cell = const_cast<WorldCell*>(find_cell_(coord));
    if (cell == nullptr || cell->residency == CellResidencyState::Unloaded) {
        return;
    }

    if (is_loading_state(cell->residency)) {
        m_load_queue.erase(std::remove_if(m_load_queue.begin(), m_load_queue.end(),
                                          [&](const LoadRequest& request) { return request.coord == coord; }),
                           m_load_queue.end());
        cell->residency = CellResidencyState::Unloaded;
        cell->visible = false;
        cell->load_priority = 0.f;
        cell->unload_priority = 0.f;
        return;
    }

    if (cell->residency == CellResidencyState::Resident ||
        cell->residency == CellResidencyState::QueuedUnload) {
        cell->residency = CellResidencyState::QueuedUnload;
        cell->unload_priority = std::max(cell->unload_priority, priority);

        const auto already_queued = std::find_if(m_unload_queue.begin(), m_unload_queue.end(),
                                                 [&](const UnloadRequest& request) {
                                                     return request.coord == coord;
                                                 });
        if (already_queued != m_unload_queue.end()) {
            already_queued->priority = std::max(already_queued->priority, priority);
            return;
        }

        m_unload_queue.push_back({coord, priority});
    }
}

bool WorldPartition::use_async_jobs_() const {
    return m_desc.async_loading && fuse::jobs::JobScheduler::instance().isInitialized() &&
           !fuse::jobs::JobScheduler::instance().isSingleThreaded();
}

StreamingWorkFn WorldPartition::make_worker_stub_() const {
    if (m_desc.cell_root.empty() || !m_staging) {
        return [](GridCoord /*coord*/, StreamingRequestKind /*kind*/) { return true; };
    }
    // Worker threads read + decode the cell file into the shared staging store; they never touch
    // the partition, the registry or the cell map (those are game-thread only).
    return [root = m_desc.cell_root, staging = m_staging](GridCoord coord, StreamingRequestKind kind) {
        if (kind != StreamingRequestKind::Load) {
            return true;
        }
        CellFileData data{};
        u64 bytes = 0;
        const CellFileStatus status = read_cell_file(cell_file_path(root, coord), data, &bytes);
        if (status == CellFileStatus::Corrupt) {
            return false;
        }
        if (status == CellFileStatus::Missing) {
            data = CellFileData{};
            data.coord = coord;
            bytes = 0;
        }
        staging->put(coord, std::move(data), bytes);
        return true;
    };
}

bool WorldPartition::take_staged_(GridCoord coord, CellFileData& out, u64& bytes) {
    return m_staging && m_staging->take(coord, out, bytes);
}

void WorldPartition::flush_async_queue_(u32 max_submits) {
    if (max_submits == 0u) {
        return;
    }
    const u32 clamped =
        clamp_pending_submits(max_submits, m_desc.budget.max_async_in_flight);
    (void)m_async_queue.flush(clamped, make_worker_stub_());
}

void WorldPartition::process_queues_() {
    const bool async_jobs = use_async_jobs_();
    if (async_jobs) {
        const u32 pending_slots =
            m_desc.budget.max_async_in_flight > m_async_queue.in_flight_count()
                ? m_desc.budget.max_async_in_flight - m_async_queue.in_flight_count()
                : 0u;
        flush_async_queue_(pending_slots);
    }

    std::sort(m_load_queue.begin(), m_load_queue.end(),
              [](const LoadRequest& a, const LoadRequest& b) { return a.priority > b.priority; });

    const u32 load_budget =
        effective_tick_budget(static_cast<u32>(m_load_queue.size()), m_desc.budget.max_loads_per_tick, false);
    u32 processed = 0;

    while (!m_load_queue.empty() && processed < load_budget) {
        const LoadRequest request = m_load_queue.front();
        m_load_queue.erase(m_load_queue.begin());

        WorldCell& cell = ensure_cell_(request.coord);
        if (is_resident_state(cell.residency)) {
            continue;
        }

        cell.residency = CellResidencyState::Loading;

        if (async_jobs) {
            StreamingRequest async_request{};
            async_request.coord = request.coord;
            async_request.kind = StreamingRequestKind::Load;
            async_request.priority = request.priority;
            (void)m_async_queue.enqueue(async_request);
        } else {
            (void)execute_load_(cell);
        }

        ++processed;
    }

    if (async_jobs && processed > 0u) {
        const u32 pending_slots =
            m_desc.budget.max_async_in_flight > m_async_queue.in_flight_count()
                ? m_desc.budget.max_async_in_flight - m_async_queue.in_flight_count()
                : 0u;
        flush_async_queue_(pending_slots);
    }

    std::sort(m_unload_queue.begin(), m_unload_queue.end(),
              [](const UnloadRequest& a, const UnloadRequest& b) { return a.priority > b.priority; });

    const u32 unload_budget =
        effective_tick_budget(static_cast<u32>(m_unload_queue.size()), m_desc.budget.max_unloads_per_tick, false);
    u32 unloaded = 0u;
    for (u32 i = 0; i < unload_budget && !m_unload_queue.empty(); ++i) {
        const UnloadRequest request = m_unload_queue.front();
        m_unload_queue.erase(m_unload_queue.begin());
        const GridCoord coord = request.coord;

        WorldCell* cell = const_cast<WorldCell*>(find_cell_(coord));
        if (cell == nullptr) {
            continue;
        }

        cell->residency = CellResidencyState::Unloading;

        if (async_jobs) {
            StreamingRequest async_request{};
            async_request.coord = coord;
            async_request.kind = StreamingRequestKind::Unload;
            async_request.priority = request.priority;
            (void)m_async_queue.enqueue(async_request);
        } else {
            execute_unload_(*cell);
        }

        ++unloaded;
    }

    if (async_jobs && unloaded > 0u) {
        const u32 pending_slots =
            m_desc.budget.max_async_in_flight > m_async_queue.in_flight_count()
                ? m_desc.budget.max_async_in_flight - m_async_queue.in_flight_count()
                : 0u;
        flush_async_queue_(pending_slots);
    }
}

void WorldPartition::drain_completed_requests_() {
    m_completed_batch_.clear();
    m_async_queue.drain_completed(m_completed_batch_);
    for (const CompletedStreamingRequest& completed : m_completed_batch_) {
        apply_completed_request_(completed);
    }

    if (use_async_jobs_()) {
        const u32 pending_slots =
            m_desc.budget.max_async_in_flight > m_async_queue.in_flight_count()
                ? m_desc.budget.max_async_in_flight - m_async_queue.in_flight_count()
                : 0u;
        flush_async_queue_(pending_slots);
    }
}

void WorldPartition::apply_completed_request_(const CompletedStreamingRequest& completed) {
    WorldCell* cell = const_cast<WorldCell*>(find_cell_(completed.coord));
    if (cell == nullptr || !completed.success) {
        if (cell != nullptr && completed.kind == StreamingRequestKind::Load &&
            cell->residency == CellResidencyState::Loading) {
            ++m_failed_loads;
            cell->residency = CellResidencyState::Unloaded;
            cell->visible = false;
        }
        return;
    }

    if (completed.kind == StreamingRequestKind::Load) {
        if (cell->residency == CellResidencyState::Loading) {
            (void)execute_load_(*cell);
        } else {
            // Superseded (cancelled or force-loaded meanwhile): drop the staged payload.
            CellFileData discarded{};
            u64 bytes = 0;
            (void)take_staged_(completed.coord, discarded, bytes);
        }
        return;
    }

    if (completed.kind == StreamingRequestKind::Unload) {
        if (cell->residency == CellResidencyState::Unloading) {
            execute_unload_(*cell);
        }
    }
}

bool WorldPartition::execute_load_(WorldCell& cell) {
    if (!m_desc.cell_root.empty()) {
        CellFileData data{};
        u64 bytes = 0;
        if (!take_staged_(cell.coord, data, bytes)) {
            // Synchronous path (sync mode, force_load, or payload superseded): read on this thread.
            const CellFileStatus status = read_cell_file(cell_file_path(m_desc.cell_root, cell.coord), data, &bytes);
            if (status == CellFileStatus::Corrupt) {
                ++m_failed_loads;
                cell.residency = CellResidencyState::Unloaded;
                cell.visible = false;
                return false;
            }
        }
        spawn_cell_entities_(cell, data);
    }

    if (m_callbacks.on_load != nullptr) {
        m_callbacks.on_load(cell);
    }
    if (cell.resident_bytes == 0u) {
        cell.resident_bytes = m_desc.default_cell_bytes;
    }
    cell.residency = CellResidencyState::Resident;
    cell.visible = true;
    touch_cell_(cell);

    const fuse::ecs::vec3 cell_center = grid_to_world_center(cell.coord, m_desc.cell_size);
    const f32 focus_distance = m_streaming.planar_distance_to(cell_center);
    (void)apply_residency_on_load_complete(m_residency_set, cell.coord, focus_distance, true);
    return true;
}

void WorldPartition::spawn_cell_entities_(WorldCell& cell, const CellFileData& data) {
    destroy_cell_entities_(cell);
    if (m_registry == nullptr) {
        return;
    }
    cell.entities.reserve(data.entities.size());
    for (const CellEntityRecord& record : data.entities) {
        const fuse::ecs::EntityID id = m_registry->create();
        if (!id.valid()) {
            continue;
        }
        fuse::ecs::Transform transform{};
        transform.position = {record.position[0], record.position[1], record.position[2], 1.f};
        transform.rotation = {record.rotation[0], record.rotation[1], record.rotation[2], record.rotation[3]};
        transform.scale = {record.scale[0], record.scale[1], record.scale[2], 0.f};
        m_registry->add<fuse::ecs::Transform>(id, transform);
        cell.entities.push_back(id);
    }
}

void WorldPartition::destroy_cell_entities_(WorldCell& cell) {
    if (m_registry != nullptr) {
        for (const fuse::ecs::EntityID id : cell.entities) {
            if (m_registry->alive(id)) {
                m_registry->destroy_entity(id);
            }
        }
    }
    cell.entities.clear();
}

void WorldPartition::execute_unload_(WorldCell& cell) {
    if (m_callbacks.on_unload != nullptr) {
        m_callbacks.on_unload(cell);
    }
    (void)apply_residency_on_unload_complete(m_residency_set, cell.coord, true);
    destroy_cell_entities_(cell);
    cell.residency = CellResidencyState::Unloaded;
    cell.visible = false;
    cell.load_priority = 0.f;
    cell.unload_priority = 0.f;
    cell.resident_bytes = 0u;
    cell.last_touch_tick = 0u;
}

void WorldPartition::collect_stream_candidates_(fuse::ecs::vec3 camera_pos) {
    const GridCoord camera_cell = world_to_grid(camera_pos, m_desc.cell_size);
    const s32 ring = static_cast<s32>(std::ceil(m_streaming.desc.stream_out_radius / m_desc.cell_size)) + 1;

    // Admit nearest cells first so a budget cap keeps the closest set (deterministic tie-break).
    m_candidates_.clear();
    for (s32 dz = -ring; dz <= ring; ++dz) {
        for (s32 dx = -ring; dx <= ring; ++dx) {
            const GridCoord coord{camera_cell.x + dx, camera_cell.y + dz};
            if (m_streaming.should_load(coord, m_desc.cell_size)) {
                m_candidates_.push_back({coord, m_streaming.load_priority_for(coord, m_desc.cell_size)});
            }
        }
    }
    std::sort(m_candidates_.begin(), m_candidates_.end(), [](const LoadRequest& a, const LoadRequest& b) {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return a.coord.y != b.coord.y ? a.coord.y < b.coord.y : a.coord.x < b.coord.x;
    });
    for (const LoadRequest& candidate : m_candidates_) {
        (void)queue_load_(candidate.coord, candidate.priority);
    }

    for (auto& entry : m_cells) {
        WorldCell& cell = entry.second;
        if (!is_resident_state(cell.residency)) {
            continue;
        }

        const fuse::ecs::vec3 cell_center = grid_to_world_center(cell.coord, m_desc.cell_size);
        const f32 focus_distance = m_streaming.planar_distance_to(cell_center);
        (void)try_update_resident_focus(m_residency_set, cell.coord, focus_distance);

        if (m_streaming.should_unload(cell.coord, m_desc.cell_size)) {
            const f32 streaming_priority = m_streaming.unload_priority_for(cell.coord, m_desc.cell_size);
            const f32 focus_distance = m_residency_set.focus_distance_for(cell.coord);
            const f32 priority =
                rank_unload_priority_guarded(streaming_priority, cell.unload_priority, focus_distance);
            queue_unload_(cell.coord, priority);
        }
    }
}

} // namespace fuse::world_partition
