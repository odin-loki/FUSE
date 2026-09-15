#pragma once

#include <fuse/terrain/math.hpp>
#include <fuse/types.hpp>

namespace fuse::terrain {

/// Residency lifecycle for a terrain clipmap chunk (B7.5 deepen).
enum class ChunkResidencyState : u8 {
    Unloaded,
    QueuedLoad,
    Loading,
    Resident,
    QueuedUnload,
    Unloading,
};

[[nodiscard]] inline bool is_resident_state(ChunkResidencyState state) {
    return state == ChunkResidencyState::Resident;
}

[[nodiscard]] inline bool is_loading_state(ChunkResidencyState state) {
    return state == ChunkResidencyState::QueuedLoad || state == ChunkResidencyState::Loading;
}

[[nodiscard]] inline bool is_unloading_state(ChunkResidencyState state) {
    return state == ChunkResidencyState::QueuedUnload || state == ChunkResidencyState::Unloading;
}

[[nodiscard]] inline bool is_transitional_state(ChunkResidencyState state) {
    return is_loading_state(state) || is_unloading_state(state);
}

[[nodiscard]] inline bool is_queued_state(ChunkResidencyState state) {
    return state == ChunkResidencyState::QueuedLoad || state == ChunkResidencyState::QueuedUnload;
}

/// Heightfield + optional underground SVO configuration (B7.5).
struct TerrainDesc {
    u32 resolution = 256;
    f32 world_size = 256.f;
    f32 max_height = 64.f;
    u32 lod_levels = 4;
    u32 chunk_resolution = 64;
    f32 load_radius = 0.f; // 0 = world_size * 0.75
    u32 max_resident_chunks = 0; // 0 = no cap
    u32 max_async_in_flight = 4;
    bool async_loading = true;
    bool has_svo_caves = true;
    u32 svo_depth = 8;
};

/// One terrain chunk in the clipmap / chunk grid (GPU handles stubbed until B2/B3 integration).
struct TerrainChunk {
    u32 lod = 0;
    f32 morph_factor = 0.f; // clipmap ring morph blend [0,1]
    ivec2 chunk_coord{};
    AABB world_bounds{};
    ChunkResidencyState residency = ChunkResidencyState::Unloaded;
    f32 load_priority = 0.f;
    u64 vertex_buffer = 0;
    u64 index_buffer = 0;
    u32 index_count = 0;
    bool dirty = false;
    bool loaded = false; // true when Resident (mirrors residency for legacy callers)
};

} // namespace fuse::terrain
