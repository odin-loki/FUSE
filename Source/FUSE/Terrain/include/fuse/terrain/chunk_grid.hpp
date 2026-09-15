#pragma once

#include <fuse/terrain/lod.hpp>
#include <fuse/terrain/lod_residency_queue.hpp>
#include <fuse/terrain/lod_residency_set.hpp>
#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::terrain {

/// Chunk grid with clipmap ring LOD transitions, morph-factor tracking, and async residency queue (B7.5 deepen).
class ChunkGrid {
public:
    void init(const TerrainDesc& desc);
    void destroy();

    [[nodiscard]] u32 chunk_count() const { return static_cast<u32>(m_chunks.size()); }
    [[nodiscard]] const TerrainChunk& chunk(u32 index) const { return m_chunks[index]; }
    [[nodiscard]] TerrainChunk& chunk(u32 index) { return m_chunks[index]; }

    [[nodiscard]] ivec2 world_to_chunk_coord(f32 world_x, f32 world_z) const;
    [[nodiscard]] AABB chunk_world_bounds(ivec2 coord, u32 lod) const;
    [[nodiscard]] f32 base_chunk_stride() const;
    [[nodiscard]] f32 effective_load_radius() const;

    /// Re-evaluate LOD rings, morph factors, and advance load/unload residency queues.
    void update_lod(vec3 camera_pos, f32 dt);

    [[nodiscard]] u32 visible_chunk_count() const;
    void collect_visible_chunks(std::vector<const TerrainChunk*>& out) const;

    [[nodiscard]] u32 resident_chunk_count() const;
    [[nodiscard]] u32 queued_load_count() const;
    [[nodiscard]] u32 queued_unload_count() const;
    [[nodiscard]] u32 in_flight_request_count() const;
    [[nodiscard]] u32 pending_completion_count() const;
    [[nodiscard]] const LodResidencySet& residency_set() const { return m_residency_set; }

    /// Apply JobScheduler completions queued since the last drain (also called from update_lod).
    u32 drain_completed_requests();

    [[nodiscard]] const TerrainDesc& desc() const { return m_desc; }
    [[nodiscard]] bool is_initialized() const { return m_initialized; }

private:
    struct LoadRequest {
        u32 chunk_index = 0;
        f32 priority = 0.f;
    };

    void rebuild_chunks();
    void drain_completed_requests_();
    void collect_stream_candidates_(vec3 camera_pos);
    void process_queues_(vec3 camera_pos);
    void queue_load_(u32 chunk_index, f32 priority);
    void queue_unload_(u32 chunk_index);
    void execute_load_(u32 chunk_index, TerrainChunk& chunk, f32 focus_distance);
    void execute_unload_(u32 chunk_index, TerrainChunk& chunk);
    void apply_completed_request_(const CompletedLodResidencyRequest& completed);
    void update_chunk_lod_(TerrainChunk& chunk, vec3 camera_pos);
    [[nodiscard]] f32 chunk_stream_distance_(ivec2 coord, vec3 camera_pos) const;
    [[nodiscard]] bool use_async_jobs_() const;
    [[nodiscard]] LodResidencyWorkFn make_worker_stub_() const;

    TerrainDesc m_desc{};
    std::vector<TerrainChunk> m_chunks;
    std::vector<LodLevel> m_lod_levels;
    std::vector<LoadRequest> m_load_queue;
    std::vector<u32> m_unload_queue;
    LodResidencyQueue m_async_queue{};
    LodResidencySet m_residency_set{};
    std::vector<CompletedLodResidencyRequest> m_completed_batch_;
    u32 m_chunks_per_axis = 0;
    bool m_initialized = false;
};

} // namespace fuse::terrain
