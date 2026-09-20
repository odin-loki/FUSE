// ConvertRGB LUV helpers for FUSE_T3D_LEGACY_ENGINE_PROBE.
// Mirrors Engine/source/core/util/rgb2luv.cpp without math/mPoint2.h / math/mPoint3.h.

#include "core/util/rgb2luv.h"

#include "core/util/rgb2xyz.h"

namespace ConvertRGB
{

LinearColorF toLUV(const LinearColorF& rgbColor)
{
    const LinearColorF xyzColor = toXYZ(rgbColor);

    const F32 x = xyzColor.red;
    const F32 y = xyzColor.green;
    const F32 z = xyzColor.blue;

    const F32 denom = x + 15.0f * y + 3.0f * z;
    F32 u = 4.0f;
    F32 v = 9.0f;
    if (denom > 0.0f) {
        u = (4.0f * x) / denom;
        v = (9.0f * y) / denom;
    }

    return LinearColorF(u, v, y, rgbColor.alpha);
}

LinearColorF toLUVScaled(const LinearColorF& rgbColor)
{
    LinearColorF luvColor = toLUV(rgbColor);
    luvColor.red /= 0.62f;
    luvColor.green /= 0.62f;
    return luvColor;
}

} // namespace ConvertRGB
