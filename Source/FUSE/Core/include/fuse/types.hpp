#pragma once

#include <cstddef>
#include <cstdint>

/// Shared host/device annotation for FUSE header-only types (math, GRIA). Under nvcc the same
/// definitions compile for both host and device; plain C++23 host compilers see nothing.
#ifndef FUSE_HOST_DEVICE
#if defined(__CUDACC__)
#define FUSE_HOST_DEVICE __host__ __device__
#else
#define FUSE_HOST_DEVICE
#endif
#endif

namespace fuse {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using i32 = s32;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

} // namespace fuse
