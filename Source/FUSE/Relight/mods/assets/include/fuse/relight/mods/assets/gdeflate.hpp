// FUSE Relight RL-3.3: GDeflate CPU codec (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.6).
//
// Remix packages store GDeflate-compressed blobs (BlobDesc::compression != 0, RTX IO's
// "GDEFLATE_1_0"): the DirectStorage GDeflate tile stream. Layout (Engine/lib/gdeflate/GDeflate/
// TileStream.h): an 8-byte header {u8 id = 4, u8 magic = id ^ 0xff, u16 numTiles, u32 tileSizeIdx:2 = 1,
// lastTileSize:18, reserved:12}, then u32 tileOffsets[numTiles] (entry 0 holds the compressed size of the
// last tile, entry i > 0 the offset of tile i), then the tiles. Every tile decodes to 64 KiB except the
// last (lastTileSize, 0 = a full tile).
//
// The codec is the vendored Apache-2.0 reference (Engine/lib/gdeflate: NVIDIA's GDeflate in libdeflate
// plus Microsoft DirectStorage's tile-stream wrapper). decompress() is FUSE's own hardened tile-stream
// walker around libdeflate_gdeflate_decompress: the header, the offset table and every tile's size are
// validated before a byte is decoded (the reference GDeflate::Decompress trusts its input), and the
// vendored decode loop carries one marked FUSE patch (Engine/lib/gdeflate/PATCHES.md, "input-bounds") so a
// corrupt tile can never make it read past the tile (found by rl_mods_assets_fuzz_asan). Built with
// FUSE_RELIGHT_GDEFLATE=OFF the codec is absent: available() is false and decompress() returns
// Status::CodecUnavailable, so package readers fall back cleanly (uncompressed blobs still load).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fuse::relight::mods::assets::gdeflate {

inline constexpr std::uint8_t kCodecId = 4;
inline constexpr std::uint32_t kTileSize = 64 * 1024;
inline constexpr std::uint32_t kMaxTiles = (1u << 16) - 1u;
inline constexpr std::size_t kHeaderSize = 8;

/// True when the vendored codec was compiled in.
bool available();

enum class Status : std::uint8_t { Ok, CodecUnavailable, BadStream, OutputSize, CorruptTile, OutOfMemory };
const char* statusName(Status s);

struct StreamInfo {
    std::uint32_t numTiles = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint64_t streamSize = 0; ///< header + offset table + tiles (bytes of `in` actually used)
};

/// Validates the header and offset table (no decoding; works without the codec).
std::optional<StreamInfo> inspect(std::span<const std::uint8_t> in, std::string* error = nullptr);

/// Decodes a whole tile stream into `out`, whose size must equal the stream's uncompressed size.
Status decompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::string* error = nullptr);
/// Convenience form; rejects streams that claim more than `maxOutput` bytes.
std::optional<std::vector<std::uint8_t>> decompress(std::span<const std::uint8_t> in, std::string* error = nullptr,
                                                    std::uint64_t maxOutput = std::uint64_t(1) << 32);

/// Compresses with the reference encoder (GDeflate::Compress, single thread, level 1..12). nullopt when
/// the codec is unavailable or the input is empty or larger than kMaxTiles tiles.
std::optional<std::vector<std::uint8_t>> compress(std::span<const std::uint8_t> in, std::uint32_t level = 9,
                                                  std::string* error = nullptr);

/// The unmodified reference decoder (GDeflate::Decompress). Test oracle only: it does not validate its
/// input, never call it on untrusted data.
bool referenceDecompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);

/// CRC-32 (IEEE, zlib polynomial) of `data`, continuing from `crc` (0 to start).
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

} // namespace fuse::relight::mods::assets::gdeflate
