#pragma once

#include <fuse/types.hpp>
#include <fuse/world_partition/grid_cell.hpp>

#include <string>
#include <vector>

namespace fuse::world_partition {

/// One entity saved in a cell file: stable identity plus transform (B7.6).
struct CellEntityRecord {
    u64 stable_id = 0;
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 rotation[4] = {0.f, 0.f, 0.f, 1.f}; ///< Quaternion (x, y, z, w)
    f32 scale[3] = {1.f, 1.f, 1.f};
};

/// Decoded contents of a binary `.fusecell` file.
struct CellFileData {
    GridCoord coord{};
    std::vector<CellEntityRecord> entities;
};

enum class CellFileStatus : u8 {
    Ok,
    Missing, ///< No file on disk (an empty cell)
    Corrupt, ///< Bad magic/version/size/checksum
};

inline constexpr u32 kCellFileMagic = 0x4C454346u; ///< "FCEL" little-endian
inline constexpr u32 kCellFileVersion = 1u;

/// Relative asset path for a cell (`cells/cell_<x>_<y>.fusecell`).
[[nodiscard]] std::string cell_asset_relative_path(GridCoord coord);

/// Serialise to the little-endian cell format: header, records, FNV-1a 64 checksum trailer.
void encode_cell_file(const CellFileData& data, std::vector<u8>& out);

/// Parse a cell buffer produced by `encode_cell_file`.
[[nodiscard]] CellFileStatus decode_cell_file(const u8* bytes, usize size, CellFileData& out);

/// Write a cell file (parent directories are created). Returns false on I/O failure.
bool write_cell_file(const std::string& path, const CellFileData& data);

/// Read and decode a cell file. `out_bytes` receives the on-disk size when non-null.
[[nodiscard]] CellFileStatus read_cell_file(const std::string& path, CellFileData& out, u64* out_bytes = nullptr);

} // namespace fuse::world_partition
