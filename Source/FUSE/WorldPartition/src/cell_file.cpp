#include <fuse/world_partition/cell_file.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fuse::world_partition {

namespace {

constexpr usize kHeaderBytes = 4u * 5u;             // magic, version, x, y, count
constexpr usize kRecordBytes = 8u + 4u * (3u + 4u + 3u); // id + position + rotation + scale
constexpr usize kTrailerBytes = 8u;

u64 fnv1a64(const u8* bytes, usize size) {
    u64 hash = 0xcbf29ce484222325ull;
    for (usize i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

void put_u32(std::vector<u8>& out, u32 value) {
    for (u32 i = 0; i < 4u; ++i) {
        out.push_back(static_cast<u8>((value >> (8u * i)) & 0xFFu));
    }
}

void put_u64(std::vector<u8>& out, u64 value) {
    for (u32 i = 0; i < 8u; ++i) {
        out.push_back(static_cast<u8>((value >> (8u * i)) & 0xFFu));
    }
}

void put_f32(std::vector<u8>& out, f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

u32 get_u32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8u) | (static_cast<u32>(p[2]) << 16u) |
           (static_cast<u32>(p[3]) << 24u);
}

u64 get_u64(const u8* p) {
    return static_cast<u64>(get_u32(p)) | (static_cast<u64>(get_u32(p + 4)) << 32u);
}

f32 get_f32(const u8* p) {
    const u32 bits = get_u32(p);
    f32 value = 0.f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

std::string cell_asset_relative_path(GridCoord coord) {
    return "cells/cell_" + std::to_string(coord.x) + "_" + std::to_string(coord.y) + ".fusecell";
}

void encode_cell_file(const CellFileData& data, std::vector<u8>& out) {
    out.clear();
    out.reserve(kHeaderBytes + data.entities.size() * kRecordBytes + kTrailerBytes);
    put_u32(out, kCellFileMagic);
    put_u32(out, kCellFileVersion);
    put_u32(out, static_cast<u32>(data.coord.x));
    put_u32(out, static_cast<u32>(data.coord.y));
    put_u32(out, static_cast<u32>(data.entities.size()));
    for (const CellEntityRecord& record : data.entities) {
        put_u64(out, record.stable_id);
        for (f32 v : record.position) {
            put_f32(out, v);
        }
        for (f32 v : record.rotation) {
            put_f32(out, v);
        }
        for (f32 v : record.scale) {
            put_f32(out, v);
        }
    }
    put_u64(out, fnv1a64(out.data(), out.size()));
}

CellFileStatus decode_cell_file(const u8* bytes, usize size, CellFileData& out) {
    if (bytes == nullptr || size < kHeaderBytes + kTrailerBytes) {
        return CellFileStatus::Corrupt;
    }
    if (get_u32(bytes) != kCellFileMagic || get_u32(bytes + 4) != kCellFileVersion) {
        return CellFileStatus::Corrupt;
    }
    const u32 count = get_u32(bytes + 16);
    if (size != kHeaderBytes + static_cast<usize>(count) * kRecordBytes + kTrailerBytes) {
        return CellFileStatus::Corrupt;
    }
    if (get_u64(bytes + size - kTrailerBytes) != fnv1a64(bytes, size - kTrailerBytes)) {
        return CellFileStatus::Corrupt;
    }

    out.coord = {static_cast<s32>(get_u32(bytes + 8)), static_cast<s32>(get_u32(bytes + 12))};
    out.entities.resize(count);
    const u8* p = bytes + kHeaderBytes;
    for (CellEntityRecord& record : out.entities) {
        record.stable_id = get_u64(p);
        p += 8;
        for (f32& v : record.position) {
            v = get_f32(p);
            p += 4;
        }
        for (f32& v : record.rotation) {
            v = get_f32(p);
            p += 4;
        }
        for (f32& v : record.scale) {
            v = get_f32(p);
            p += 4;
        }
    }
    return CellFileStatus::Ok;
}

bool write_cell_file(const std::string& path, const CellFileData& data) {
    std::error_code ec;
    const std::filesystem::path file_path(path);
    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path(), ec);
    }
    std::vector<u8> bytes;
    encode_cell_file(data, bytes);
    std::ofstream stream(file_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

CellFileStatus read_cell_file(const std::string& path, CellFileData& out, u64* out_bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return CellFileStatus::Missing;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (out_bytes != nullptr) {
        *out_bytes = bytes.size();
    }
    return decode_cell_file(bytes.data(), bytes.size(), out);
}

} // namespace fuse::world_partition
