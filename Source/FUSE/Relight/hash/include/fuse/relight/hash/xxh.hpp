// FUSE Relight: thin wrappers over the vendored xxHash 0.8.x (Engine/lib/xxhash, BSD-2-Clause).
//
// Remix-compatible hashes (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1) use two xxHash functions:
// XXH3_64bits (optionally seeded) and XXH64. Their output has been frozen since xxHash 0.8.0, the
// version dxvk-remix @0867d3c bundles. xxhash.h itself is kept private to the library.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fuse::relight::hash {

/// A 64-bit asset hash (XXH64_hash_t upstream).
using Hash64 = std::uint64_t;

/// Upstream `kEmptyHash`: the value of a component that was not computed.
inline constexpr Hash64 kEmptyHash = 0;

/// XXH3_64bits(data, size). `data` may be null when `size` is 0.
[[nodiscard]] Hash64 xxh3_64(const void* data, std::size_t size) noexcept;

/// XXH3_64bits_withSeed(data, size, seed). Seed 0 gives the same value as the unseeded call.
[[nodiscard]] Hash64 xxh3_64(const void* data, std::size_t size, Hash64 seed) noexcept;

/// XXH64(data, size, seed).
[[nodiscard]] Hash64 xxh64(const void* data, std::size_t size, Hash64 seed) noexcept;

/// XXH_versionNumber() of the linked xxHash (MAJOR*10000 + MINOR*100 + RELEASE).
[[nodiscard]] unsigned xxhashVersionNumber() noexcept;

} // namespace fuse::relight::hash
