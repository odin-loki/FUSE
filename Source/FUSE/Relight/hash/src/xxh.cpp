// FUSE Relight: xxHash wrappers. xxHash is compiled inline into this translation unit only
// (XXH_INLINE_ALL gives every symbol internal linkage), so it never clashes with another copy of
// xxHash linked into the same process (for example a game's own).
//
// XXH_FORCE_MEMORY_ACCESS 0 makes every unaligned read a memcpy. xxHash's GCC default (1) reads
// through aligned(1) integer typedefs, which is not aliasing-safe once the hash is inlined into a
// caller that has just written the bytes as floats (seen at -O2 in the upstream-oracle test).
#include <fuse/relight/hash/xxh.hpp>

#include <bit>

#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <fuse_xxhash_version.h>

static_assert(std::endian::native == std::endian::little, "Remix hashes are defined over little-endian bytes");
static_assert(XXH_VERSION_NUMBER >= 800, "Remix-compatible hashes need xxHash >= 0.8.0 (frozen XXH3 output)");

namespace fuse::relight::hash {

Hash64 xxh3_64(const void* data, std::size_t size) noexcept {
    return XXH3_64bits(data, size);
}

Hash64 xxh3_64(const void* data, std::size_t size, Hash64 seed) noexcept {
    return XXH3_64bits_withSeed(data, size, seed);
}

Hash64 xxh64(const void* data, std::size_t size, Hash64 seed) noexcept {
    return XXH64(data, size, seed);
}

unsigned xxhashVersionNumber() noexcept {
    return XXH_versionNumber();
}

} // namespace fuse::relight::hash
