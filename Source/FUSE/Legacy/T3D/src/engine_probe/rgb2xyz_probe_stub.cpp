// ConvertRGB sRGB↔XYZ helpers for FUSE_T3D_LEGACY_ENGINE_PROBE.
// Mirrors Engine/source/core/util/rgb2xyz.cpp without math/mMatrix.h (math_backend).

#include "core/util/rgb2xyz.h"

namespace ConvertRGB
{

namespace
{

struct Mat4
{
    F32 m[16];
};

void mulMat4Point4(const Mat4& mat, LinearColorF& color)
{
    const F32 x = color.red;
    const F32 y = color.green;
    const F32 z = color.blue;
    const F32 w = color.alpha;

    color.red = mat.m[0] * x + mat.m[1] * y + mat.m[2] * z + mat.m[3] * w;
    color.green = mat.m[4] * x + mat.m[5] * y + mat.m[6] * z + mat.m[7] * w;
    color.blue = mat.m[8] * x + mat.m[9] * y + mat.m[10] * z + mat.m[11] * w;
    color.alpha = mat.m[12] * x + mat.m[13] * y + mat.m[14] * z + mat.m[15] * w;
}

// http://www.w3.org/Graphics/Color/sRGB — same constants as rgb2xyz.cpp
const Mat4 scRGB2XYZ = {
    0.4124f, 0.3576f, 0.1805f, 0.0f, 0.2126f, 0.7152f, 0.0722f, 0.0f,
    0.0193f, 0.1192f, 0.9505f, 0.0f, 0.0f,    0.0f,    0.0f,    1.0f,
};

const Mat4 scXYZ2RGB = {
    3.2410f,  -1.5374f, -0.4986f, 0.0f, -0.9692f, 1.8760f,  0.0416f,  0.0f,
    0.0556f,  -0.2040f, 1.0570f,  0.0f, 0.0f,     0.0f,     0.0f,     1.0f,
};

} // namespace

LinearColorF toXYZ(const LinearColorF& rgbColor)
{
    LinearColorF retColor = rgbColor;
    mulMat4Point4(scRGB2XYZ, retColor);
    return retColor;
}

LinearColorF fromXYZ(const LinearColorF& xyzColor)
{
    LinearColorF retColor = xyzColor;
    mulMat4Point4(scXYZ2RGB, retColor);
    return retColor;
}

} // namespace ConvertRGB
