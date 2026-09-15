#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/world_partition/grid_cell.hpp>
#include <fuse/world_partition/streaming_volume.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::world_partition {

struct WorldPartitionDesc {
    f32 cell_size = 256.f;
    f32 stream_in_distance = 512.f;
    f32 stream_out_distance = 600.f;
    u32 max_loaded_cells = 64;
    bool async_loading = true;
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

    /// Re-evaluate streaming volume and advance load/unload queues.
    void update(fuse::ecs::vec3 camera_pos);

    void force_load(GridCoord coord);
    void force_unload(GridCoord coord);

    [[nodiscard]] bool cell_loaded(GridCoord coord) const;
    [[nodiscard]] CellResidencyState cell_residency(GridCoord coord) const;
    [[nodiscard]] u32 loaded_cell_count() const;
    [[nodiscard]] u32 resident_cell_count() const;
    [[nodiscard]] const WorldCell* find_cell(GridCoord coord) const;
    [[nodiscard]] const WorldPartitionDesc& desc() const { return m_desc; }

private:
    struct LoadRequest {
        GridCoord coord{};
        f32 priority = 0.f;
    };

    WorldCell& ensure_cell_(GridCoord coord);
    [[nodiscard]] const WorldCell* find_cell_(GridCoord coord) const;
    void queue_load_(GridCoord coord, f32 priority);
    void queue_unload_(GridCoord coord);
    void process_queues_();
    void execute_load_(WorldCell& cell);
    void execute_unload_(WorldCell& cell);
    void collect_stream_candidates_(fuse::ecs::vec3 camera_pos);

    WorldPartitionDesc m_desc{};
    CellLoadCallbacks m_callbacks{};
    std::unordered_map<u64, WorldCell> m_cells;
    std::vector<LoadRequest> m_load_queue;
    std::vector<GridCoord> m_unload_queue;
    StreamingVolume m_streaming{};
};

} // namespace fuse::world_partition
