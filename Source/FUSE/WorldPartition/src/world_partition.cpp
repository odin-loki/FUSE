#include <fuse/world_partition/world_partition.hpp>

#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace fuse::world_partition {

void WorldPartition::init(const WorldPartitionDesc& desc) {
    destroy();
    m_desc = desc;
    m_streaming.desc.stream_in_radius = desc.stream_in_distance;
    m_streaming.desc.stream_out_radius = desc.stream_out_distance;
}

void WorldPartition::destroy() {
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
    m_callbacks = {};
    m_tick = 0;
    m_budget_counters = {};
}

void WorldPartition::update(fuse::ecs::vec3 camera_pos) {
    ++m_tick;
    drain_completed_requests_();
    m_streaming.center = camera_pos;
    collect_stream_candidates_(camera_pos);
    process_queues_();
}

void WorldPartition::force_load(GridCoord coord) {
    (void)queue_load_(coord, std::numeric_limits<f32>::max());
    process_queues_();
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
    cell.asset_path = "cells/cell_" + std::to_string(coord.x) + "_" + std::to_string(coord.y) + ".fusecell";
    const auto inserted = m_cells.emplace(key, std::move(cell));
    return inserted.first->second;
}

const WorldCell* WorldPartition::find_cell_(GridCoord coord) const {
    const auto it = m_cells.find(grid_coord_key(coord));
    return it != m_cells.end() ? &it->second : nullptr;
}

bool WorldPartition::can_accept_load_(u64 incoming_bytes) const {
    if (!can_accept_resident_cell(m_desc.max_loaded_cells, resident_cell_count())) {
        return false;
    }
    return !would_exceed_byte_budget(m_desc.budget.max_resident_bytes, resident_byte_count(), incoming_bytes);
}

void WorldPartition::touch_cell_(WorldCell& cell) {
    cell.last_touch_tick = m_tick;
}

f32 WorldPartition::eviction_score_for_(const WorldCell& cell) const {
    const f32 distance_priority =
        m_streaming.unload_priority_for(cell.coord, m_desc.cell_size);
    return eviction_score_for(distance_priority, cell.last_touch_tick, m_tick, m_desc.eviction_policy);
}

f32 WorldPartition::budget_eviction_score_for_(const WorldCell& cell) const {
    const f32 focus_distance = m_residency_set.focus_distance_for(cell.coord);
    const f32 distance_priority =
        m_streaming.unload_priority_for(cell.coord, m_desc.cell_size);
    return budget_eviction_score(focus_distance, distance_priority, cell.last_touch_tick, m_tick,
                                 m_desc.eviction_policy);
}

void WorldPartition::evict_for_budget_(f32 incoming_priority) {
    while (!can_accept_load_(m_desc.default_cell_bytes)) {
        WorldCell* best_candidate = nullptr;
        f32 best_score = -1.f;

        if (m_desc.eviction_policy == EvictionPolicy::DistanceFromFocus && m_residency_set.has_eviction_candidate()) {
            const GridCoord eviction_coord = m_residency_set.pick_eviction_candidate();
            best_candidate = const_cast<WorldCell*>(find_cell_(eviction_coord));
            if (best_candidate != nullptr) {
                best_score = budget_eviction_score_for_(*best_candidate);
            }
        } else {
            for (auto& entry : m_cells) {
                WorldCell& cell = entry.second;
                if (!is_resident_state(cell.residency)) {
                    continue;
                }

                const f32 score = budget_eviction_score_for_(cell);
                if (score > best_score) {
                    best_score = score;
                    best_candidate = &cell;
                }
            }
        }

        if (best_candidate == nullptr || best_score <= 0.f) {
            ++m_budget_counters.eviction_skipped;
            break;
        }

        if (incoming_priority > 0.f && incoming_priority < std::numeric_limits<f32>::max() &&
            best_score > incoming_priority && m_desc.eviction_policy == EvictionPolicy::DistanceFromFocus) {
            ++m_budget_counters.eviction_skipped;
            break;
        }

        const f32 focus_distance = m_residency_set.focus_distance_for(best_candidate->coord);
        const f32 streaming_priority =
            m_streaming.unload_priority_for(best_candidate->coord, m_desc.cell_size);
        const f32 unload_priority =
            rank_unload_priority_stub(streaming_priority, best_candidate->unload_priority, focus_distance);

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

    const u64 incoming_bytes = cell.resident_bytes > 0u ? cell.resident_bytes : m_desc.default_cell_bytes;
    if (!can_accept_load_(incoming_bytes)) {
        evict_for_budget_(priority);
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
    return [](GridCoord /*coord*/, StreamingRequestKind /*kind*/) {
        // Production wiring reads binary cell assets from disk on worker threads.
        return true;
    };
}

void WorldPartition::process_queues_() {
    std::sort(m_load_queue.begin(), m_load_queue.end(),
              [](const LoadRequest& a, const LoadRequest& b) { return a.priority > b.priority; });

    const bool async_jobs = use_async_jobs_();
    const u32 load_budget =
        effective_tick_budget(static_cast<u32>(m_load_queue.size()), m_desc.budget.max_loads_per_tick, false);
    u32 processed = 0;

    while (!m_load_queue.empty() && resident_cell_count() < m_desc.max_loaded_cells && processed < load_budget) {
        if (async_jobs && m_async_queue.in_flight_count() >= m_desc.budget.max_async_in_flight) {
            break;
        }

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
            if (!m_async_queue.submit(async_request, make_worker_stub_())) {
                execute_load_(cell);
            }
        } else {
            execute_load_(cell);
        }

        ++processed;
    }

    std::sort(m_unload_queue.begin(), m_unload_queue.end(),
              [](const UnloadRequest& a, const UnloadRequest& b) { return a.priority > b.priority; });

    const u32 unload_budget =
        effective_tick_budget(static_cast<u32>(m_unload_queue.size()), m_desc.budget.max_unloads_per_tick, false);
    for (u32 i = 0; i < unload_budget && !m_unload_queue.empty(); ++i) {
        if (async_jobs && m_async_queue.in_flight_count() >= m_desc.budget.max_async_in_flight) {
            break;
        }

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
            if (!m_async_queue.submit(async_request, make_worker_stub_())) {
                execute_unload_(*cell);
            }
        } else {
            execute_unload_(*cell);
        }
    }
}

void WorldPartition::drain_completed_requests_() {
    m_completed_batch_.clear();
    m_async_queue.drain_completed(m_completed_batch_);
    for (const CompletedStreamingRequest& completed : m_completed_batch_) {
        apply_completed_request_(completed);
    }
}

void WorldPartition::apply_completed_request_(const CompletedStreamingRequest& completed) {
    WorldCell* cell = const_cast<WorldCell*>(find_cell_(completed.coord));
    if (cell == nullptr || !completed.success) {
        if (cell != nullptr && completed.kind == StreamingRequestKind::Load) {
            cell->residency = CellResidencyState::Unloaded;
            cell->visible = false;
        }
        return;
    }

    if (completed.kind == StreamingRequestKind::Load) {
        if (cell->residency == CellResidencyState::Loading) {
            execute_load_(*cell);
        }
        return;
    }

    if (completed.kind == StreamingRequestKind::Unload) {
        if (cell->residency == CellResidencyState::Unloading) {
            execute_unload_(*cell);
        }
    }
}

void WorldPartition::execute_load_(WorldCell& cell) {
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
    (void)try_add_resident(m_residency_set, cell.coord, focus_distance);
}

void WorldPartition::execute_unload_(WorldCell& cell) {
    if (m_callbacks.on_unload != nullptr) {
        m_callbacks.on_unload(cell);
    }
    (void)try_remove_resident(m_residency_set, cell.coord);
    cell.entities.clear();
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

    for (s32 dz = -ring; dz <= ring; ++dz) {
        for (s32 dx = -ring; dx <= ring; ++dx) {
            const GridCoord coord{camera_cell.x + dx, camera_cell.y + dz};
            if (m_streaming.should_load(coord, m_desc.cell_size)) {
                const f32 priority = m_streaming.load_priority_for(coord, m_desc.cell_size);
                (void)queue_load_(coord, priority);
            }
        }
    }

    for (auto& entry : m_cells) {
        WorldCell& cell = entry.second;
        if (!is_resident_state(cell.residency)) {
            continue;
        }

        const fuse::ecs::vec3 cell_center = grid_to_world_center(cell.coord, m_desc.cell_size);
        const f32 focus_distance = m_streaming.planar_distance_to(cell_center);
        (void)m_residency_set.update_focus_distance(cell.coord, focus_distance);

        if (m_streaming.should_unload(cell.coord, m_desc.cell_size)) {
            const f32 streaming_priority = m_streaming.unload_priority_for(cell.coord, m_desc.cell_size);
            const f32 focus_distance = m_residency_set.focus_distance_for(cell.coord);
            const f32 priority =
                rank_unload_priority_stub(streaming_priority, cell.unload_priority, focus_distance);
            queue_unload_(cell.coord, priority);
        }
    }
}

} // namespace fuse::world_partition
