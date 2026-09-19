#include <fuse/legacy/t3d/image_compress.hpp>
#include <fuse/legacy/parallel_for.hpp>

#include <squish.h>

#include <cstdint>

namespace fuse::legacy::t3d::image {

namespace {

int squishFormatFlags(CompressFormat format) {
    switch (format) {
    case CompressFormat::BC1:
        return squish::kDxt1;
    case CompressFormat::BC3:
        return squish::kDxt5;
    }
    return squish::kDxt1;
}

int squishQualityFlags(CompressQuality quality) {
    switch (quality) {
    case CompressQuality::Low:
        return squish::kColourRangeFit;
    case CompressQuality::Medium:
        return squish::kColourClusterFit;
    case CompressQuality::High:
        return squish::kColourIterativeClusterFit;
    }
    return squish::kColourRangeFit;
}

bool rawCompress(const u8* srcRGBA, u8* dst, u32 width, u32 height, CompressFormat format, CompressQuality quality) {
    if (!srcRGBA || !dst || width == 0 || height == 0) {
        return false;
    }

    const int flags = squishFormatFlags(format) | squishQualityFlags(quality);
    squish::CompressImage(srcRGBA, static_cast<int>(width), static_cast<int>(height), dst, flags);
    return true;
}

} // namespace

u32 compressedMipByteCount(u32 width, u32 height, CompressFormat format) {
    const int flags = squishFormatFlags(format);
    return static_cast<u32>(squish::GetStorageRequirements(static_cast<int>(width), static_cast<int>(height), flags));
}

bool compressMipsParallel(MipLevel* mips, u32 mipCount, CompressFormat format, CompressQuality quality) {
    if (!mips || mipCount == 0) {
        return false;
    }

    fuse::legacy::parallel_for_indices(0u, mipCount, 1u, [&](u32 mipIndex) {
        MipLevel& mip = mips[mipIndex];
        rawCompress(mip.srcRGBA, mip.dst, mip.width, mip.height, format, quality);
    });
    return true;
}

u32 compressedMipsChecksum(const MipLevel* mips, u32 mipCount, CompressFormat format) {
    if (!mips || mipCount == 0) {
        return 0;
    }

    u32 checksum = 0;
    for (u32 mipIndex = 0; mipIndex < mipCount; ++mipIndex) {
        const MipLevel& mip = mips[mipIndex];
        const u32 byteCount = compressedMipByteCount(mip.width, mip.height, format);
        for (u32 i = 0; i < byteCount; ++i) {
            checksum = (checksum * 131u) + static_cast<u32>(mip.dst[i]);
        }
    }
    return checksum;
}

} // namespace fuse::legacy::t3d::image
