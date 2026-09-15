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
    m_callbacks = {};
}

void WorldPartition::update(fuse::ecs::vec3 camera_pos) {
    drain_completed_requests_();
    m_streaming.center = camera_pos;
    collect_stream_candidates_(camera_pos);
    process_queues_();
}

void WorldPartition::force_load(GridCoord coord) {
    queue_load_(coord, std::numeric_limits<f32>::max());
    process_queues_();
}

void WorldPartition::force_unload(GridCoord coord) {
    queue_unload_(coord);
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

u32 WorldPartition::queued_load_count() const { return static_cast<u32>(m_load_queue.size()); }

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

void WorldPartition::queue_load_(GridCoord coord, f32 priority) {
    WorldCell& cell = ensure_cell_(coord);
    if (is_resident_state(cell.residency) || is_loading_state(cell.residency)) {
        cell.load_priority = std::max(cell.load_priority, priority);
        return;
    }

    cell.residency = CellResidencyState::QueuedLoad;
    cell.load_priority = priority;

    const auto already_queued = std::find_if(m_load_queue.begin(), m_load_queue.end(),
                                             [&](const LoadRequest& request) {
                                                 return request.coord == coord;
                                             });
    if (already_queued != m_load_queue.end()) {
        already_queued->priority = std::max(already_queued->priority, priority);
        return;
    }

    m_load_queue.push_back({coord, priority});
}

void WorldPartition::queue_unload_(GridCoord coord) {
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
        return;
    }

    if (cell->residency == CellResidencyState::Resident) {
        cell->residency = CellResidencyState::QueuedUnload;
        if (std::find(m_unload_queue.begin(), m_unload_queue.end(), coord) == m_unload_queue.end()) {
            m_unload_queue.push_back(coord);
        }
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
    const u32 max_per_tick = async_jobs ? m_desc.max_async_in_flight : static_cast<u32>(m_load_queue.size());
    u32 processed = 0;

    while (!m_load_queue.empty() && resident_cell_count() < m_desc.max_loaded_cells && processed < max_per_tick) {
        if (async_jobs && m_async_queue.in_flight_count() >= m_desc.max_async_in_flight) {
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

    const u32 unload_budget = async_jobs ? m_desc.max_async_in_flight : static_cast<u32>(m_unload_queue.size());
    for (u32 i = 0; i < unload_budget && !m_unload_queue.empty(); ++i) {
        if (async_jobs && m_async_queue.in_flight_count() >= m_desc.max_async_in_flight) {
            break;
        }

        const GridCoord coord = m_unload_queue.front();
        m_unload_queue.erase(m_unload_queue.begin());

        WorldCell* cell = const_cast<WorldCell*>(find_cell_(coord));
        if (cell == nullptr) {
            continue;
        }

        cell->residency = CellResidencyState::Unloading;

        if (async_jobs) {
            StreamingRequest async_request{};
            async_request.coord = coord;
            async_request.kind = StreamingRequestKind::Unload;
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
    cell.residency = CellResidencyState::Resident;
    cell.visible = true;
}

void WorldPartition::execute_unload_(WorldCell& cell) {
    if (m_callbacks.on_unload != nullptr) {
        m_callbacks.on_unload(cell);
    }
    cell.entities.clear();
    cell.residency = CellResidencyState::Unloaded;
    cell.visible = false;
    cell.load_priority = 0.f;
}

void WorldPartition::collect_stream_candidates_(fuse::ecs::vec3 camera_pos) {
    const GridCoord camera_cell = world_to_grid(camera_pos, m_desc.cell_size);
    const s32 ring = static_cast<s32>(std::ceil(m_streaming.desc.stream_out_radius / m_desc.cell_size)) + 1;

    for (s32 dz = -ring; dz <= ring; ++dz) {
        for (s32 dx = -ring; dx <= ring; ++dx) {
            const GridCoord coord{camera_cell.x + dx, camera_cell.y + dz};
            if (m_streaming.should_load(coord, m_desc.cell_size)) {
                const f32 priority = m_streaming.load_priority_for(coord, m_desc.cell_size);
                queue_load_(coord, priority);
            }
        }
    }

    for (auto& entry : m_cells) {
        WorldCell& cell = entry.second;
        if (!is_resident_state(cell.residency)) {
            continue;
        }
        if (m_streaming.should_unload(cell.coord, m_desc.cell_size)) {
            queue_unload_(cell.coord);
        }
    }
}

} // namespace fuse::world_partition
