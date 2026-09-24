#pragma once

// WP-5.2: chunk-level view of an FMLT (`.fusemeshlet`) file. The DAG writer uses it to append its
// chunks to the WP-1.2 1.0 bytes without re-encoding them; tests use it to build files with
// unknown, duplicate, missing or resized chunks. No semantic validation here: split checks only
// that the header, table and payload ranges lie inside the buffer (parse_* functions validate).

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::geometry::dag {

constexpr u32 fmlt_fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

inline constexpr u32 kFmltHeaderBytes = 64u;
inline constexpr u32 kFmltChunkEntryBytes = 32u;
inline constexpr u32 kFmltChunkAlign = 16u;
inline constexpr u32 kFmltTrailerBytes = 8u;

struct FmltChunk {
    u32 fourcc = 0;
    u32 element_bytes = 0;
    u32 element_count = 0;
    std::vector<u8> payload; ///< element_bytes * element_count bytes (assemble writes it as given)
};

struct FmltFile {
    std::vector<u8> header; ///< the 64 header bytes (chunk_count is rewritten by assemble)
    std::vector<FmltChunk> chunks;
};

/// Split into header + chunks (table order). Fails on a short buffer or out-of-range chunk.
bool split_fmlt(const u8* data, usize size, FmltFile& out, std::string* error = nullptr);
/// Header, table, 16-byte aligned payloads in order (zero padding), FNV-1a 64 trailer.
[[nodiscard]] std::vector<u8> assemble_fmlt(const FmltFile& file);

/// Header field helpers (little-endian, offsets per meshlet_format.hpp).
[[nodiscard]] u16 fmlt_version_minor(const FmltFile& file);
void fmlt_set_version_minor(FmltFile& file, u16 minor);
void fmlt_set_version_major(FmltFile& file, u16 major);

/// Index of the first chunk with `fourcc`, or -1.
[[nodiscard]] s32 fmlt_find_chunk(const FmltFile& file, u32 fourcc);

} // namespace fuse::renderer::geometry::dag
