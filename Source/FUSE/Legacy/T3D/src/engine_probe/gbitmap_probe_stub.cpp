// Minimal GBitmap subset for FUSE_T3D_LEGACY_ENGINE_PROBE (bitmapSTB.cpp).
// Implements only allocation, registration, and transparency helpers — not full gBitmap.cpp.

#include "core/util/path.h"
#include "gfx/bitmap/gBitmap.h"

#include "platform/platformAssert.h"

#include <algorithm>

const U32 GBitmap::csFileVersion = 5;

Vector<GBitmap::Registration>& GBitmap::getRegistrations() {
    static Vector<GBitmap::Registration>* regs = new Vector<GBitmap::Registration>(__FILE__, __LINE__);
    return *regs;
}

GBitmap::GBitmap()
    : mInternalFormat(GFXFormatR8G8B8), mBytesPerPixel(0), mHasTransparency(false) {
    VECTOR_SET_ASSOCIATION(mFaces);
    mFaces.setSize(1);
}

GBitmap::~GBitmap() {
    deleteImage();
}

U32 GBitmap::getFormatBytesPerPixel(GFXFormat fmt) {
    switch (fmt) {
    case GFXFormatA8:
    case GFXFormatL8:
    case GFXFormatA4L4:
        return 1;
    case GFXFormatR5G6B5:
    case GFXFormatR5G5B5A1:
    case GFXFormatR5G5B5X1:
    case GFXFormatA8L8:
    case GFXFormatL16:
    case GFXFormatR16F:
    case GFXFormatD16:
        return 2;
    case GFXFormatR8G8B8:
    case GFXFormatR8G8B8_SRGB:
        return 3;
    case GFXFormatR8G8B8A8:
    case GFXFormatR8G8B8X8:
    case GFXFormatB8G8R8A8:
    case GFXFormatR8G8B8A8_SRGB:
    case GFXFormatR32F:
    case GFXFormatR10G10B10A2:
    case GFXFormatR11G11B10:
    case GFXFormatD24X8:
    case GFXFormatD24S8:
    case GFXFormatD24FS8:
    case GFXFormatR16G16:
    case GFXFormatR16G16F:
    case GFXFormatR8G8B8A8_LINEAR_FORCE:
        return 4;
    case GFXFormatR16G16B16A16:
    case GFXFormatR16G16B16A16F:
    case GFXFormatD32FS8X24:
        return 8;
    case GFXFormatR32G32B32A32F:
        return 16;
    default:
        AssertWarn(false, "getFormatBytesPerPixel() - Unknown or compressed format");
        return 4;
    }
}

void GBitmap::sRegisterFormat(const Registration& reg) {
    U32 insert = getRegistrations().size();
    for (U32 i = 0; i < getRegistrations().size(); i++) {
        if (getRegistrations()[i].priority <= reg.priority) {
            insert = i;
            break;
        }
    }
    getRegistrations().insert(insert, reg);
}

const GBitmap::Registration* GBitmap::sFindRegInfo(const String& extension) {
    for (U32 i = 0; i < getRegistrations().size(); i++) {
        const Registration& reg = getRegistrations()[i];
        for (U32 j = 0; j < reg.extensions.size(); ++j) {
            if (reg.extensions[j].equal(extension, String::NoCase)) {
                return &reg;
            }
        }
    }
    return nullptr;
}

void GBitmap::deleteImage() {
    mFaces.setSize(1);
    mFaces[0].deleteImage();
}

void GBitmap::allocateBitmap(const U32 in_width,
                             const U32 in_height,
                             const bool in_extrudeMipLevels,
                             const GFXFormat in_format,
                             const U32 in_numFaces) {
    if (in_extrudeMipLevels) {
        AssertFatal(isPow2(in_width) && isPow2(in_height),
                    "GBitmap::allocateBitmap: extrude requires pow2 dimensions");
    }
    allocateBitmapWithMips(in_width, in_height, in_extrudeMipLevels ? 0 : 1, in_format, in_numFaces);
}

void GBitmap::allocateBitmapWithMips(const U32 in_width,
                                     const U32 in_height,
                                     const U32 in_numMips,
                                     const GFXFormat in_format,
                                     const U32 in_numFaces) {
    AssertFatal(in_width != 0 && in_height != 0, "GBitmap::allocateBitmapWithMips: width or height is 0");
    AssertFatal(in_numFaces >= 1, "GBitmap::allocateBitmapWithMips: in_numFaces must be at least 1");

    mInternalFormat = in_format;
    mBytesPerPixel = getFormatBytesPerPixel(mInternalFormat);
    mFaces.setSize(in_numFaces);
    for (U32 i = 0; i < in_numFaces; i++) {
        mFaces[i].allocate(in_width, in_height, in_numMips, mBytesPerPixel);
    }
}

U32 GBitmap::getByteSize() const {
    U32 total = 0;
    for (U32 i = 0; i < mFaces.size(); i++) {
        total += mFaces[i].getByteSize();
    }
    return total;
}

bool GBitmap::checkForTransparency() {
    mHasTransparency = false;
    if (getFaceByteSize() == 0) {
        return false;
    }

    switch (mInternalFormat) {
    case GFXFormatA8:
    case GFXFormatA4L4:
    case GFXFormatA8L8:
    case GFXFormatR5G5B5A1:
    case GFXFormatR8G8B8A8:
    case GFXFormatB8G8R8A8:
    case GFXFormatR8G8B8A8_SRGB:
    case GFXFormatR10G10B10A2:
    case GFXFormatR16G16B16A16:
    case GFXFormatR16G16B16A16F:
    case GFXFormatR32G32B32A32F:
        break;
    default:
        return false;
    }

    const U8* bits = getBits();
    const U32 bpp = getBytesPerPixel();
    if (bpp == 0) {
        return false;
    }

    const U32 alphaIndex = (bpp >= 4) ? 3u : (bpp == 2 ? 1u : 0u);
    for (U32 offset = alphaIndex; offset < getByteSize(); offset += bpp) {
        if (bits[offset] < 255) {
            mHasTransparency = true;
            break;
        }
    }
    return mHasTransparency;
}

GBitmap::Face::Face()
    : mBits(nullptr), mByteSize(0), mWidth(0), mHeight(0), mBytesPerPixel(0), mNumMipLevels(0) {
    dMemset(mMipLevelOffsets, 0, sizeof(mMipLevelOffsets));
}

GBitmap::Face::~Face() {
    delete[] mBits;
}

void GBitmap::Face::deleteImage() {
    delete[] mBits;
    mBits = nullptr;
    mByteSize = 0;
    mWidth = 0;
    mHeight = 0;
    mNumMipLevels = 0;
}

void GBitmap::Face::allocate(const U32 in_width,
                             const U32 in_height,
                             const U32 in_numMips,
                             const U32 in_bytesPerPixel) {
    AssertFatal(in_width != 0 && in_height != 0, "GBitmap::Face::allocate: width or height is 0");

    delete[] mBits;

    mWidth = in_width;
    mHeight = in_height;
    mBytesPerPixel = in_bytesPerPixel;
    mNumMipLevels = 1;
    mMipLevelOffsets[0] = 0;

    U32 currWidth = in_width;
    U32 currHeight = in_height;
    while ((currWidth != 1 || currHeight != 1) && (in_numMips == 0 || mNumMipLevels < in_numMips)) {
        mMipLevelOffsets[mNumMipLevels] =
            mMipLevelOffsets[mNumMipLevels - 1] + (currWidth * currHeight * mBytesPerPixel);
        currWidth >>= 1;
        currHeight >>= 1;
        if (currWidth == 0) {
            currWidth = 1;
        }
        if (currHeight == 0) {
            currHeight = 1;
        }
        mNumMipLevels++;
    }

    AssertFatal(mNumMipLevels <= c_maxMipLevels, "GBitmap::Face::allocate: too many miplevels");

    mByteSize = 0;
    for (U32 mip = 0; mip < mNumMipLevels; mip++) {
        mByteSize += getWidth(mip) * getHeight(mip) * mBytesPerPixel;
    }

    mBits = new U8[mByteSize];
    dMemset(mBits, 0xFF, mByteSize);
}

U32 GBitmap::Face::getWidth(const U32 mipLevel) const {
    AssertFatal(mipLevel < mNumMipLevels, "GBitmap::Face::getWidth: mip level out of range");
    const U32 retVal = mWidth >> mipLevel;
    return retVal != 0 ? retVal : 1;
}

U32 GBitmap::Face::getHeight(const U32 mipLevel) const {
    AssertFatal(mipLevel < mNumMipLevels, "GBitmap::Face::getHeight: mip level out of range");
    const U32 retVal = mHeight >> mipLevel;
    return retVal != 0 ? retVal : 1;
}

const U8* GBitmap::Face::getBits(const U32 mipLevel) const {
    AssertFatal(mipLevel < mNumMipLevels, "GBitmap::Face::getBits: mip level out of range");
    return &mBits[mMipLevelOffsets[mipLevel]];
}

U8* GBitmap::Face::getWritableBits(const U32 mipLevel) {
    AssertFatal(mipLevel < mNumMipLevels, "GBitmap::Face::getWritableBits: mip level out of range");
    return &mBits[mMipLevelOffsets[mipLevel]];
}
