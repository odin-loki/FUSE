#pragma once

#include <fuse/types.hpp>

namespace fuse::legacy::t3d::image {

enum class CompressFormat : u8 {
    BC1 = 0,
    BC3 = 1,
};

enum class CompressQuality : u8 {
    Low = 0,
    Medium = 1,
    High = 2,
};

/// One mip level — disjoint src/dst buffers (Tier A, WP-11 P1).
struct MipLevel {
    const u8* srcRGBA = nullptr;
    u8* dst = nullptr;
    u32 width = 0;
    u32 height = 0;
};

/// Returns required dst byte count for a BC block-compressed mip.
u32 compressedMipByteCount(u32 width, u32 height, CompressFormat format);

/// Parallel-compress mip levels via `fuse::legacy::parallel_for_indices` (replaces ThreadPool::CompressJob).
bool compressMipsParallel(MipLevel* mips, u32 mipCount, CompressFormat format, CompressQuality quality);

/// Deterministic checksum over compressed mip payloads — smoke parity gate.
u32 compressedMipsChecksum(const MipLevel* mips, u32 mipCount, CompressFormat format);

} // namespace fuse::legacy::t3d::image
