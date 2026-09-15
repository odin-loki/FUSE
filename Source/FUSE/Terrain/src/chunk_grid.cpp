#include <fuse/terrain/chunk_grid.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/terrain/lod_residency_budget.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace fuse::terrain {

void ChunkGrid::init(const TerrainDesc& desc) {
    destroy();
    m_desc = desc;
    m_lod_levels.clear();
    for (u32 level = 0; level < desc.lod_levels; ++level) {
        m_lod_levels.push_back(make_lod_level(desc, level));
    }

    m_chunks_per_axis = std::max(1u, desc.resolution / std::max(desc.chunk_resolution, 1u));
    m_async_queue.set_max_pending_submits(desc.max_async_in_flight);
    rebuild_chunks();
    m_initialized = true;
}

void ChunkGrid::destroy() {
    drain_completed_requests_();
    for (int attempt = 0; attempt < 1000 && m_async_queue.in_flight_count() > 0; ++attempt) {
        drain_completed_requests_();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (u32 index = 0; index < m_chunks.size(); ++index) {
        TerrainChunk& chunk = m_chunks[index];
        if (is_resident_state(chunk.residency) || is_transitional_state(chunk.residency)) {
            execute_unload_(index, chunk);
        }
    }
    m_chunks.clear();
    m_residency_set.clear();
    m_budget_counters = {};
    m_lod_levels.clear();
    m_load_queue.clear();
    m_unload_queue.clear();
    m_completed_batch_.clear();
    m_async_queue.clear();
    m_initialized = false;
}

ivec2 ChunkGrid::world_to_chunk_coord(f32 world_x, f32 world_z) const {
    const f32 chunk_size = m_desc.world_size / static_cast<f32>(m_chunks_per_axis);
    const s32 x = static_cast<s32>(std::floor(world_x / chunk_size));
    const s32 z = static_cast<s32>(std::floor(world_z / chunk_size));
    return {x, z};
}

f32 ChunkGrid::base_chunk_stride() const {
    return m_desc.world_size / static_cast<f32>(std::max(m_chunks_per_axis, 1u));
}

f32 ChunkGrid::effective_load_radius() const {
    return m_desc.load_radius > 0.f ? m_desc.load_radius : m_desc.world_size * 0.75f;
}

AABB ChunkGrid::chunk_world_bounds(ivec2 coord, u32 lod) const {
    const LodLevel& level = m_lod_levels[std::min(lod, static_cast<u32>(m_lod_levels.size()) - 1)];
    const f32 chunk_size = m_desc.world_size / static_cast<f32>(m_chunks_per_axis) * level.world_stride;

    const f32 min_x = static_cast<f32>(coord.x) * chunk_size;
    const f32 min_z = static_cast<f32>(coord.y) * chunk_size;
    return {
        {min_x, 0.f, min_z},
        {min_x + chunk_size, m_desc.max_height, min_z + chunk_size},
    };
}

void ChunkGrid::update_lod(vec3 camera_pos, f32 /*dt*/) {
    if (!m_initialized) {
        return;
    }

    drain_completed_requests_();

    for (TerrainChunk& chunk : m_chunks) {
        update_chunk_lod_(chunk, camera_pos);
    }

    collect_stream_candidates_(camera_pos);
    process_queues_(camera_pos);
}

u32 ChunkGrid::visible_chunk_count() const {
    u32 count = 0;
    for (const TerrainChunk& chunk : m_chunks) {
        if (chunk.loaded) {
            ++count;
        }
    }
    return count;
}

void ChunkGrid::collect_visible_chunks(std::vector<const TerrainChunk*>& out) const {
    out.clear();
    for (const TerrainChunk& chunk : m_chunks) {
        if (chunk.loaded) {
            out.push_back(&chunk);
        }
    }
}

u32 ChunkGrid::resident_chunk_count() const {
    u32 count = 0;
    for (const TerrainChunk& chunk : m_chunks) {
        if (is_resident_state(chunk.residency)) {
            ++count;
        }
    }
    return count;
}

u32 ChunkGrid::queued_load_count() const { return static_cast<u32>(m_load_queue.size()); }

u32 ChunkGrid::queued_unload_count() const { return static_cast<u32>(m_unload_queue.size()); }

u32 ChunkGrid::in_flight_request_count() const { return m_async_queue.in_flight_count(); }

u32 ChunkGrid::pending_completion_count() const { return m_async_queue.completed_count(); }

u32 ChunkGrid::drain_completed_requests() {
    drain_completed_requests_();
    return static_cast<u32>(m_completed_batch_.size());
}

void ChunkGrid::rebuild_chunks() {
    m_chunks.clear();
    m_chunks.reserve(static_cast<usize>(m_chunks_per_axis) * static_cast<usize>(m_chunks_per_axis));

    for (u32 z = 0; z < m_chunks_per_axis; ++z) {
        for (u32 x = 0; x < m_chunks_per_axis; ++x) {
            TerrainChunk chunk{};
            chunk.chunk_coord = {static_cast<s32>(x), static_cast<s32>(z)};
            chunk.lod = 0;
            chunk.world_bounds = chunk_world_bounds(chunk.chunk_coord, chunk.lod);
            chunk.residency = ChunkResidencyState::Unloaded;
            chunk.loaded = false;
            m_chunks.push_back(chunk);
        }
    }
}

f32 ChunkGrid::chunk_stream_distance_(ivec2 coord, vec3 camera_pos) const {
    const f32 base_size = m_desc.world_size / static_cast<f32>(m_chunks_per_axis);
    const f32 centre_x = (static_cast<f32>(coord.x) + 0.5f) * base_size;
    const f32 centre_z = (static_cast<f32>(coord.y) + 0.5f) * base_size;
    const f32 dx = centre_x - camera_pos.x;
    const f32 dz = centre_z - camera_pos.z;
    return std::sqrt(dx * dx + dz * dz);
}

void ChunkGrid::update_chunk_lod_(TerrainChunk& chunk, vec3 camera_pos) {
    const f32 distance = chunk_stream_distance_(chunk.chunk_coord, camera_pos);

    const LodTransition transition = compute_lod_transition(distance, m_desc.lod_levels);
    const u32 clamped_lod = clamp_lod_level(transition.lod, m_desc.lod_levels);
    const bool lod_changed = chunk.lod != clamped_lod;
    chunk.lod = clamped_lod;
    chunk.morph_factor = clamp_morph_factor(transition.morph_factor);
    chunk.world_bounds = chunk_world_bounds(chunk.chunk_coord, transition.lod);
    chunk.dirty = chunk.loaded && (lod_changed || chunk.morph_factor > 0.f);
}

void ChunkGrid::collect_stream_candidates_(vec3 camera_pos) {
    const f32 load_radius = effective_load_radius();
    const f32 unload_radius = load_radius * 1.25f;

    for (u32 index = 0; index < m_chunks.size(); ++index) {
        TerrainChunk& chunk = m_chunks[index];
        const f32 distance = chunk_stream_distance_(chunk.chunk_coord, camera_pos);

        if (is_resident_state(chunk.residency)) {
            m_residency_set.update_focus_distance(index, distance);
        }

        if (distance < load_radius) {
            const f32 priority = load_radius - distance;
            queue_load_(index, priority);
        } else if (distance > unload_radius && is_resident_state(chunk.residency)) {
            queue_unload_(index);
        }
    }
}

void ChunkGrid::evict_for_resident_cap_(f32 incoming_priority) {
    const f32 load_radius = effective_load_radius();
    while (is_at_resident_cap(m_desc.max_resident_chunks, resident_chunk_count())) {
        const u32 eviction_index = m_residency_set.pick_eviction_candidate();
        if (eviction_index == kInvalidChunkIndex) {
            break;
        }

        const f32 resident_focus_distance = m_residency_set.focus_distance_for(eviction_index);
        if (!incoming_outranks_resident(incoming_priority, load_radius, resident_focus_distance)) {
            break;
        }

        ++m_budget_counters.budget_evictions;
        queue_unload_(eviction_index);
    }
}

void ChunkGrid::queue_load_(u32 chunk_index, f32 priority) {
    if (chunk_index >= m_chunks.size()) {
        return;
    }

    TerrainChunk& chunk = m_chunks[chunk_index];
    if (is_resident_state(chunk.residency) || is_loading_state(chunk.residency)) {
        chunk.load_priority = promote_residency_priority(chunk.load_priority, priority);
        return;
    }

    if (!can_accept_resident_chunk(m_desc.max_resident_chunks, resident_chunk_count())) {
        evict_for_resident_cap_(priority);
    }
    if (!can_accept_resident_chunk(m_desc.max_resident_chunks, resident_chunk_count())) {
        ++m_budget_counters.rejected_loads;
        return;
    }

    chunk.residency = ChunkResidencyState::QueuedLoad;
    chunk.load_priority = promote_residency_priority(chunk.load_priority, priority);

    const auto already_queued = std::find_if(m_load_queue.begin(), m_load_queue.end(),
                                             [&](const LoadRequest& request) {
                                                 return request.chunk_index == chunk_index;
                                             });
    if (already_queued != m_load_queue.end()) {
        already_queued->priority = promote_residency_priority(already_queued->priority, priority);
        return;
    }

    m_load_queue.push_back({chunk_index, priority});
}

void ChunkGrid::queue_unload_(u32 chunk_index) {
    if (chunk_index >= m_chunks.size()) {
        return;
    }

    TerrainChunk& chunk = m_chunks[chunk_index];
    if (chunk.residency == ChunkResidencyState::Unloaded) {
        return;
    }

    if (is_loading_state(chunk.residency)) {
        m_load_queue.erase(std::remove_if(m_load_queue.begin(), m_load_queue.end(),
                                          [&](const LoadRequest& request) {
                                              return request.chunk_index == chunk_index;
                                          }),
                           m_load_queue.end());
        chunk.residency = ChunkResidencyState::Unloaded;
        chunk.loaded = false;
        chunk.load_priority = 0.f;
        return;
    }

    if (chunk.residency == ChunkResidencyState::Resident) {
        chunk.residency = ChunkResidencyState::QueuedUnload;
        if (std::find(m_unload_queue.begin(), m_unload_queue.end(), chunk_index) == m_unload_queue.end()) {
            m_unload_queue.push_back(chunk_index);
        }
    }
}

bool ChunkGrid::use_async_jobs_() const {
    return m_desc.async_loading && fuse::jobs::JobScheduler::instance().isInitialized() &&
           !fuse::jobs::JobScheduler::instance().isSingleThreaded();
}

LodResidencyWorkFn ChunkGrid::make_worker_stub_() const {
    return [](u32 /*chunk_index*/, LodResidencyRequestKind /*kind*/) {
        // Production wiring reads chunk mesh/heightfield assets from disk on worker threads.
        return true;
    };
}

void ChunkGrid::process_queues_(vec3 camera_pos) {
    std::sort(m_load_queue.begin(), m_load_queue.end(),
              [](const LoadRequest& a, const LoadRequest& b) { return a.priority > b.priority; });

    const bool async_jobs = use_async_jobs_();
    const u32 max_per_tick =
        async_jobs ? m_desc.max_async_in_flight : effective_tick_budget(static_cast<u32>(m_load_queue.size()), 0u);
    u32 processed = 0;

    while (!m_load_queue.empty() && can_accept_resident_chunk(m_desc.max_resident_chunks, resident_chunk_count()) &&
           processed < max_per_tick) {
        if (async_jobs && m_async_queue.in_flight_count() >= m_desc.max_async_in_flight) {
            break;
        }

        const LoadRequest request = m_load_queue.front();
        m_load_queue.erase(m_load_queue.begin());

        TerrainChunk& chunk = m_chunks[request.chunk_index];
        if (is_resident_state(chunk.residency)) {
            continue;
        }

        const f32 focus_distance = chunk_stream_distance_(chunk.chunk_coord, camera_pos);
        chunk.residency = ChunkResidencyState::Loading;

        if (async_jobs) {
            LodResidencyRequest async_request{};
            async_request.chunk_index = request.chunk_index;
            async_request.kind = LodResidencyRequestKind::Load;
            async_request.priority = request.priority;
            async_request.morph_snapshot = capture_morph_snapshot(chunk.lod, chunk.morph_factor);
            if (!m_async_queue.submit(async_request, make_worker_stub_())) {
                execute_load_(request.chunk_index, chunk, focus_distance);
            }
        } else {
            execute_load_(request.chunk_index, chunk, focus_distance);
        }

        ++processed;
    }

    const u32 unload_budget = async_jobs ? m_desc.max_async_in_flight : static_cast<u32>(m_unload_queue.size());
    for (u32 i = 0; i < unload_budget && !m_unload_queue.empty(); ++i) {
        if (async_jobs && m_async_queue.in_flight_count() >= m_desc.max_async_in_flight) {
            break;
        }

        const u32 chunk_index = m_unload_queue.front();
        m_unload_queue.erase(m_unload_queue.begin());

        if (chunk_index >= m_chunks.size()) {
            continue;
        }

        TerrainChunk& chunk = m_chunks[chunk_index];
        chunk.residency = ChunkResidencyState::Unloading;

        if (async_jobs) {
            LodResidencyRequest async_request{};
            async_request.chunk_index = chunk_index;
            async_request.kind = LodResidencyRequestKind::Unload;
            async_request.morph_snapshot = capture_morph_snapshot(chunk.lod, chunk.morph_factor);
            if (!m_async_queue.submit(async_request, make_worker_stub_())) {
                execute_unload_(chunk_index, chunk);
            }
        } else {
            execute_unload_(chunk_index, chunk);
        }
    }
}

void ChunkGrid::drain_completed_requests_() {
    m_completed_batch_.clear();
    m_async_queue.drain_completed(m_completed_batch_);
    for (const CompletedLodResidencyRequest& completed : m_completed_batch_) {
        apply_completed_request_(completed);
    }
}

void ChunkGrid::apply_completed_request_(const CompletedLodResidencyRequest& completed) {
    if (completed.chunk_index >= m_chunks.size()) {
        return;
    }

    TerrainChunk& chunk = m_chunks[completed.chunk_index];
    if (!completed.success) {
        if (completed.kind == LodResidencyRequestKind::Load) {
            chunk.residency = ChunkResidencyState::Unloaded;
            chunk.loaded = false;
        }
        return;
    }

    if (completed.kind == LodResidencyRequestKind::Load) {
        if (chunk.residency == ChunkResidencyState::Loading) {
            const f32 focus_distance = chunk_stream_distance_(chunk.chunk_coord, {0.f, 0.f, 0.f});
            execute_load_(completed.chunk_index, chunk, focus_distance);
            sync_morph_after_residency(chunk, completed.morph_snapshot);
        }
        return;
    }

    if (completed.kind == LodResidencyRequestKind::Unload) {
        if (chunk.residency == ChunkResidencyState::Unloading) {
            execute_unload_(completed.chunk_index, chunk);
            chunk.morph_factor = 0.f;
        }
    }
}

void ChunkGrid::execute_load_(u32 chunk_index, TerrainChunk& chunk, f32 focus_distance) {
    chunk.residency = ChunkResidencyState::Resident;
    chunk.loaded = true;
    m_residency_set.add(chunk_index, focus_distance);
}

void ChunkGrid::execute_unload_(u32 chunk_index, TerrainChunk& chunk) {
    chunk.residency = ChunkResidencyState::Unloaded;
    chunk.loaded = false;
    chunk.load_priority = 0.f;
    chunk.dirty = false;
    m_residency_set.remove(chunk_index);
}

} // namespace fuse::terrain
