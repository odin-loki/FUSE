#include <fuse/renderer/geometry/dag/fmlt_chunks.hpp>

#include <fuse/renderer/geometry/meshlet_format.hpp>

namespace fuse::renderer::geometry::dag {

namespace {

u32 read_u32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

u64 read_u64(const u8* p) { return static_cast<u64>(read_u32(p)) | (static_cast<u64>(read_u32(p + 4)) << 32); }

void put_u32(std::vector<u8>& out, u32 v) {
    for (u32 s = 0; s < 32u; s += 8u) {
        out.push_back(static_cast<u8>((v >> s) & 0xFFu));
    }
}

void put_u64(std::vector<u8>& out, u64 v) {
    put_u32(out, static_cast<u32>(v & 0xFFFFFFFFu));
    put_u32(out, static_cast<u32>(v >> 32));
}

void patch_u16(std::vector<u8>& bytes, usize offset, u16 v) {
    if (bytes.size() >= offset + 2u) {
        bytes[offset] = static_cast<u8>(v & 0xFFu);
        bytes[offset + 1u] = static_cast<u8>(v >> 8);
    }
}

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = "fmlt chunks: " + message;
    }
    return false;
}

} // namespace

bool split_fmlt(const u8* data, usize size, FmltFile& out, std::string* error) {
    out = FmltFile{};
    if (data == nullptr || size < kFmltHeaderBytes + kFmltTrailerBytes) {
        return fail(error, "shorter than header + trailer");
    }
    const u32 chunkCount = read_u32(data + 16);
    const u64 tableEnd = kFmltHeaderBytes + static_cast<u64>(chunkCount) * kFmltChunkEntryBytes;
    if (tableEnd + kFmltTrailerBytes > size) {
        return fail(error, "chunk table past the end");
    }
    out.header.assign(data, data + kFmltHeaderBytes);
    out.chunks.resize(chunkCount);
    for (u32 c = 0; c < chunkCount; ++c) {
        const u8* e = data + kFmltHeaderBytes + static_cast<usize>(c) * kFmltChunkEntryBytes;
        FmltChunk& chunk = out.chunks[c];
        chunk.fourcc = read_u32(e);
        chunk.element_bytes = read_u32(e + 4);
        chunk.element_count = read_u32(e + 8);
        const u64 offset = read_u64(e + 16);
        const u64 bytes = read_u64(e + 24);
        if (offset > size || bytes > size - kFmltTrailerBytes || offset + bytes > size - kFmltTrailerBytes) {
            out = FmltFile{};
            return fail(error, "chunk " + std::to_string(c) + " payload out of range");
        }
        chunk.payload.assign(data + offset, data + offset + bytes);
    }
    return true;
}

std::vector<u8> assemble_fmlt(const FmltFile& file) {
    std::vector<u8> out(file.header.begin(), file.header.end());
    out.resize(kFmltHeaderBytes, 0u);
    const u32 count = static_cast<u32>(file.chunks.size());
    for (u32 s = 0; s < 32u; s += 8u) {
        out[16u + s / 8u] = static_cast<u8>((count >> s) & 0xFFu);
    }
    u64 cursor = kFmltHeaderBytes + static_cast<u64>(count) * kFmltChunkEntryBytes;
    std::vector<u64> offsets(count);
    for (u32 c = 0; c < count; ++c) {
        cursor = (cursor + kFmltChunkAlign - 1u) / kFmltChunkAlign * kFmltChunkAlign;
        offsets[c] = cursor;
        cursor += file.chunks[c].payload.size();
    }
    out.reserve(static_cast<usize>(cursor) + kFmltChunkAlign + kFmltTrailerBytes);
    for (u32 c = 0; c < count; ++c) {
        const FmltChunk& chunk = file.chunks[c];
        put_u32(out, chunk.fourcc);
        put_u32(out, chunk.element_bytes);
        put_u32(out, chunk.element_count);
        put_u32(out, 0u);
        put_u64(out, offsets[c]);
        put_u64(out, chunk.payload.size());
    }
    for (u32 c = 0; c < count; ++c) {
        out.resize(static_cast<usize>(offsets[c]), 0u);
        out.insert(out.end(), file.chunks[c].payload.begin(), file.chunks[c].payload.end());
    }
    put_u64(out, meshlet_fnv1a64(out.data(), out.size()));
    return out;
}

u16 fmlt_version_minor(const FmltFile& file) {
    return file.header.size() >= 8u ? static_cast<u16>(file.header[6] | (file.header[7] << 8)) : u16{0};
}

void fmlt_set_version_minor(FmltFile& file, u16 minor) { patch_u16(file.header, 6u, minor); }

void fmlt_set_version_major(FmltFile& file, u16 major) { patch_u16(file.header, 4u, major); }

s32 fmlt_find_chunk(const FmltFile& file, u32 fourcc) {
    for (usize i = 0; i < file.chunks.size(); ++i) {
        if (file.chunks[i].fourcc == fourcc) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

} // namespace fuse::renderer::geometry::dag
