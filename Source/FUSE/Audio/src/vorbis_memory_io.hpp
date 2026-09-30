#pragma once

// Internal: vorbisfile callbacks over an in-memory Ogg stream (shared by audio_codec.cpp and
// audio_stream.cpp). Only compiled with the vendored xiph libraries (FUSE_HAS_OGG_VORBIS).

#if defined(FUSE_HAS_OGG_VORBIS)

#include <fuse/types.hpp>

#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fuse::audio::detail {

struct MemoryReader {
    const u8* data = nullptr;
    usize size = 0;
    usize pos = 0;
};

inline size_t mem_read(void* ptr, size_t size, size_t nmemb, void* source) {
    auto* r = static_cast<MemoryReader*>(source);
    if (size == 0 || r->pos >= r->size) {
        return 0;
    }
    const usize items = std::min<usize>(nmemb, (r->size - r->pos) / size);
    std::memcpy(ptr, r->data + r->pos, items * size);
    r->pos += items * size;
    return items;
}

inline int mem_seek(void* source, ogg_int64_t offset, int whence) {
    auto* r = static_cast<MemoryReader*>(source);
    ogg_int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = static_cast<ogg_int64_t>(r->pos);
    } else if (whence == SEEK_END) {
        base = static_cast<ogg_int64_t>(r->size);
    }
    const ogg_int64_t target = base + offset;
    if (target < 0 || target > static_cast<ogg_int64_t>(r->size)) {
        return -1;
    }
    r->pos = static_cast<usize>(target);
    return 0;
}

inline long mem_tell(void* source) {
    return static_cast<long>(static_cast<MemoryReader*>(source)->pos);
}

inline ov_callbacks memory_callbacks() {
    ov_callbacks cb{};
    cb.read_func = &mem_read;
    cb.seek_func = &mem_seek;
    cb.close_func = nullptr;
    cb.tell_func = &mem_tell;
    return cb;
}

} // namespace fuse::audio::detail

#endif
