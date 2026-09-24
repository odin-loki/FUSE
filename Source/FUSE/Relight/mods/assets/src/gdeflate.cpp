// FUSE Relight RL-3.3: GDeflate tile-stream codec around the vendored reference (see gdeflate.hpp).
#include <fuse/relight/mods/assets/gdeflate.hpp>

#include <algorithm>
#include <array>
#include <memory>

#if FUSE_RELIGHT_HAS_GDEFLATE
#include <GDeflate.h>
#include <libdeflate.h>

#include <fuse_gdeflate_version.h>
#endif

namespace fuse::relight::mods::assets::gdeflate {

namespace {

std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

void setError(std::string* error, std::string why) {
    if (error) {
        *error = std::move(why);
    }
}

std::array<std::uint32_t, 256> makeCrcTable() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }
        t[i] = c;
    }
    return t;
}

#if FUSE_RELIGHT_HAS_GDEFLATE
struct TileSpan {
    std::uint64_t offset, size;
};

/// Offset/size of tile `i` (validated by inspect()).
TileSpan tileSpan(std::span<const std::uint8_t> in, std::uint32_t numTiles, std::uint32_t i) {
    const std::uint8_t* table = in.data() + kHeaderSize;
    const std::uint64_t off = i == 0 ? 0 : get32(table + 4 * i);
    const std::uint64_t end = i + 1 < numTiles ? get32(table + 4 * (i + 1)) : off + get32(table);
    return {off, end - off};
}

struct DecompressorDeleter {
    void operator()(libdeflate_gdeflate_decompressor* d) const { libdeflate_free_gdeflate_decompressor(d); }
};
#endif

} // namespace

bool available() { return FUSE_RELIGHT_HAS_GDEFLATE != 0; }

const char* statusName(Status s) {
    switch (s) {
    case Status::Ok:
        return "ok";
    case Status::CodecUnavailable:
        return "GDeflate codec unavailable (built with FUSE_RELIGHT_GDEFLATE=OFF)";
    case Status::BadStream:
        return "malformed GDeflate stream";
    case Status::OutputSize:
        return "output size does not match the stream";
    case Status::CorruptTile:
        return "corrupt GDeflate tile";
    case Status::OutOfMemory:
        return "out of memory";
    }
    return "?";
}

std::optional<StreamInfo> inspect(std::span<const std::uint8_t> in, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<StreamInfo> {
        setError(error, why);
        return std::nullopt;
    };
    if (in.size() < kHeaderSize) {
        return fail("GDeflate stream shorter than its header");
    }
    const std::uint8_t id = in[0], magic = in[1];
    if (magic != static_cast<std::uint8_t>(id ^ 0xffu) || id != kCodecId) {
        return fail("not a GDeflate tile stream (id / magic)");
    }
    StreamInfo info;
    info.numTiles = std::uint32_t(in[2]) | (std::uint32_t(in[3]) << 8);
    const std::uint32_t word = get32(in.data() + 4);
    const std::uint32_t tileSizeIdx = word & 3u, lastTileSize = (word >> 2) & 0x3ffffu;
    if (tileSizeIdx != 1) {
        return fail("unsupported GDeflate tile size index");
    }
    if (info.numTiles == 0 || lastTileSize >= kTileSize) {
        return fail("bad GDeflate tile count or last tile size");
    }
    info.uncompressedSize = std::uint64_t(info.numTiles) * kTileSize - (lastTileSize ? kTileSize - lastTileSize : 0);
    const std::uint64_t dataStart = kHeaderSize + 4ull * info.numTiles;
    if (in.size() < dataStart) {
        return fail("GDeflate tile offset table truncated");
    }
    const std::uint8_t* table = in.data() + kHeaderSize;
    std::uint64_t prev = 0;
    for (std::uint32_t i = 1; i < info.numTiles; ++i) {
        const std::uint64_t off = get32(table + 4 * i);
        if (off <= prev) {
            return fail("GDeflate tile offsets are not increasing");
        }
        prev = off;
    }
    const std::uint64_t lastSize = get32(table);
    if (lastSize == 0) {
        return fail("empty last GDeflate tile");
    }
    info.streamSize = dataStart + prev + lastSize;
    if (info.streamSize > in.size()) {
        return fail("GDeflate tiles extend past the end of the stream");
    }
    return info;
}

Status decompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::string* error) {
    const auto info = inspect(in, error);
    if (!info) {
        return Status::BadStream;
    }
    if (out.size() != info->uncompressedSize) {
        setError(error, "output buffer is " + std::to_string(out.size()) + " bytes, the stream decodes to " +
                            std::to_string(info->uncompressedSize));
        return Status::OutputSize;
    }
#if FUSE_RELIGHT_HAS_GDEFLATE
    std::unique_ptr<libdeflate_gdeflate_decompressor, DecompressorDeleter> d(libdeflate_alloc_gdeflate_decompressor());
    if (!d) {
        setError(error, statusName(Status::OutOfMemory));
        return Status::OutOfMemory;
    }
    const std::uint8_t* data = in.data() + kHeaderSize + 4ull * info->numTiles;
    for (std::uint32_t i = 0; i < info->numTiles; ++i) {
        const TileSpan t = tileSpan(in, info->numTiles, i);
        const std::uint64_t outOffset = std::uint64_t(i) * kTileSize;
        const std::size_t expected = static_cast<std::size_t>(std::min<std::uint64_t>(kTileSize, info->uncompressedSize - outOffset));
        libdeflate_gdeflate_in_page page{data + t.offset, static_cast<std::size_t>(t.size)};
        std::size_t actual = 0;
        const libdeflate_result r =
            libdeflate_gdeflate_decompress(d.get(), &page, 1, out.data() + outOffset, expected, &actual);
        if (r != LIBDEFLATE_SUCCESS || actual != expected) {
            setError(error, "GDeflate tile " + std::to_string(i) + " is corrupt (libdeflate result " + std::to_string(int(r)) +
                                ", " + std::to_string(actual) + " of " + std::to_string(expected) + " bytes)");
            return Status::CorruptTile;
        }
    }
    return Status::Ok;
#else
    setError(error, statusName(Status::CodecUnavailable));
    return Status::CodecUnavailable;
#endif
}

std::optional<std::vector<std::uint8_t>> decompress(std::span<const std::uint8_t> in, std::string* error,
                                                    std::uint64_t maxOutput) {
    const auto info = inspect(in, error);
    if (!info) {
        return std::nullopt;
    }
    if (info->uncompressedSize > maxOutput) {
        setError(error, "GDeflate stream decodes to more than the allowed " + std::to_string(maxOutput) + " bytes");
        return std::nullopt;
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(info->uncompressedSize));
    if (decompress(in, out, error) != Status::Ok) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> compress(std::span<const std::uint8_t> in, std::uint32_t level, std::string* error) {
#if FUSE_RELIGHT_HAS_GDEFLATE
    if (in.empty() || in.size() > std::uint64_t(kTileSize) * kMaxTiles) {
        setError(error, "GDeflate input must be 1 byte .. 65535 tiles");
        return std::nullopt;
    }
    if (level < GDeflate::MinimumCompressionLevel || level > GDeflate::MaximumCompressionLevel) {
        setError(error, "GDeflate compression level must be 1..12");
        return std::nullopt;
    }
    std::vector<std::uint8_t> out(GDeflate::CompressBound(in.size()));
    std::size_t size = out.size();
    if (!GDeflate::Compress(out.data(), &size, in.data(), in.size(), level, GDeflate::COMPRESS_SINGLE_THREAD)) {
        setError(error, "GDeflate compression failed");
        return std::nullopt;
    }
    out.resize(size);
    return out;
#else
    (void)in;
    (void)level;
    setError(error, statusName(Status::CodecUnavailable));
    return std::nullopt;
#endif
}

bool referenceDecompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
#if FUSE_RELIGHT_HAS_GDEFLATE
    return GDeflate::Decompress(out.data(), out.size(), in.data(), in.size(), 1);
#else
    (void)in;
    (void)out;
    return false;
#endif
}

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    static const std::array<std::uint32_t, 256> table = makeCrcTable();
    crc = ~crc;
    for (const std::uint8_t b : data) {
        crc = table[(crc ^ b) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace fuse::relight::mods::assets::gdeflate
