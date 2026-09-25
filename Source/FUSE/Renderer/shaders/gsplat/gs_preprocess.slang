// WP-9.2 "gsplat.preprocess". Slang twin of gs_preprocess.comp (see there).
#include "gs_common.slang"

static const float kShC0 = 0.28209479177387814;
static const float kShC1 = 0.4886025119029199;
static const float kShC20 = 1.0925484305920792;
static const float kShC21 = -1.0925484305920792;
static const float kShC22 = 0.31539156525252005;
static const float kShC23 = -1.0925484305920792;
static const float kShC24 = 0.5462742152960396;
static const float kShC30 = -0.5900435899266435;
static const float kShC31 = 2.890611442640554;
static const float kShC32 = -0.4570457994644658;
static const float kShC33 = 0.3731763325901154;
static const float kShC34 = -0.4570457994644658;
static const float kShC35 = 1.445305721320277;
static const float kShC36 = -0.5900435899266435;

float gsShChannel(uint base, uint c, uint degree, float x, float y, float z) {
    float* s = gsFloats(F.splats);
    const uint sh = base + 12u + c;
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float yz = y * z;
    const float xz = x * z;
    float res = kShC0 * s[sh + 0u];
    if (degree > 0u) {
        res = res - kShC1 * y * s[sh + 3u] + kShC1 * z * s[sh + 6u] - kShC1 * x * s[sh + 9u];
        if (degree > 1u) {
            res = res + kShC20 * xy * s[sh + 12u] + kShC21 * yz * s[sh + 15u] +
                  kShC22 * (2.0 * zz - xx - yy) * s[sh + 18u] + kShC23 * xz * s[sh + 21u] +
                  kShC24 * (xx - yy) * s[sh + 24u];
            if (degree > 2u) {
                res = res + kShC30 * y * (3.0 * xx - yy) * s[sh + 27u] + kShC31 * xy * z * s[sh + 30u] +
                      kShC32 * y * (4.0 * zz - xx - yy) * s[sh + 33u] +
                      kShC33 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy) * s[sh + 36u] +
                      kShC34 * x * (4.0 * zz - xx - yy) * s[sh + 39u] + kShC35 * z * (xx - yy) * s[sh + 42u] +
                      kShC36 * x * (xx - 3.0 * yy) * s[sh + 45u];
            }
        }
    }
    res = res + 0.5;
    return res > 0.0 ? res : 0.0;
}

[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    gsBegin();
    const uint i = tid.x;
    if (i >= F.splatCount) {
        return;
    }
    uint4* outp = gsProjectedPtr(F.projected);
    float* s = gsFloats(F.splats);
    const uint base = i * kGsSplatFloats;
    const float px = s[base + 0u];
    const float py = s[base + 1u];
    const float pz = s[base + 2u];
    const float tx = F.view[0] * px + F.view[1] * py + F.view[2] * pz + F.view[3];
    const float ty = F.view[4] * px + F.view[5] * py + F.view[6] * pz + F.view[7];
    const float tz = F.view[8] * px + F.view[9] * py + F.view[10] * pz + F.view[11];
    bool culled = !(tz > F.nearZ);
    uint4 w0 = uint4(0u), w1 = uint4(0u), w2 = uint4(0u), w3 = uint4(0u);
    if (!culled) {
        const float qw = s[base + 8u];
        const float qx = s[base + 9u];
        const float qy = s[base + 10u];
        const float qz = s[base + 11u];
        const float sx = s[base + 4u];
        const float sy = s[base + 5u];
        const float sz = s[base + 6u];
        const float m00 = (1.0 - 2.0 * (qy * qy + qz * qz)) * sx;
        const float m01 = (2.0 * (qx * qy - qw * qz)) * sy;
        const float m02 = (2.0 * (qx * qz + qw * qy)) * sz;
        const float m10 = (2.0 * (qx * qy + qw * qz)) * sx;
        const float m11 = (1.0 - 2.0 * (qx * qx + qz * qz)) * sy;
        const float m12 = (2.0 * (qy * qz - qw * qx)) * sz;
        const float m20 = (2.0 * (qx * qz - qw * qy)) * sx;
        const float m21 = (2.0 * (qy * qz + qw * qx)) * sy;
        const float m22 = (1.0 - 2.0 * (qx * qx + qy * qy)) * sz;
        const float s00 = m00 * m00 + m01 * m01 + m02 * m02;
        const float s01 = m00 * m10 + m01 * m11 + m02 * m12;
        const float s02 = m00 * m20 + m01 * m21 + m02 * m22;
        const float s11 = m10 * m10 + m11 * m11 + m12 * m12;
        const float s12 = m10 * m20 + m11 * m21 + m12 * m22;
        const float s22 = m20 * m20 + m21 * m21 + m22 * m22;
        const float limx = 1.3 * F.tanFovX;
        const float limy = 1.3 * F.tanFovY;
        const float cxz = gsClamp(tx / tz, -limx, limx) * tz;
        const float cyz = gsClamp(ty / tz, -limy, limy) * tz;
        const float tz2 = tz * tz;
        const float j00 = F.fx / tz;
        const float j02 = -(F.fx * cxz) / tz2;
        const float j11 = F.fy / tz;
        const float j12 = -(F.fy * cyz) / tz2;
        const float t00 = j00 * F.view[0] + j02 * F.view[8];
        const float t01 = j00 * F.view[1] + j02 * F.view[9];
        const float t02 = j00 * F.view[2] + j02 * F.view[10];
        const float t10 = j11 * F.view[4] + j12 * F.view[8];
        const float t11 = j11 * F.view[5] + j12 * F.view[9];
        const float t12 = j11 * F.view[6] + j12 * F.view[10];
        const float u00 = t00 * s00 + t01 * s01 + t02 * s02;
        const float u01 = t00 * s01 + t01 * s11 + t02 * s12;
        const float u02 = t00 * s02 + t01 * s12 + t02 * s22;
        const float u10 = t10 * s00 + t11 * s01 + t12 * s02;
        const float u11 = t10 * s01 + t11 * s11 + t12 * s12;
        const float u12 = t10 * s02 + t11 * s12 + t12 * s22;
        const float a = u00 * t00 + u01 * t01 + u02 * t02 + F.lowPass;
        const float b = u00 * t10 + u01 * t11 + u02 * t12;
        const float c = u10 * t10 + u11 * t11 + u12 * t12 + F.lowPass;
        const float det = a * c - b * b;
        culled = !(det > 0.0);
        if (!culled) {
            const float inv = 1.0 / det;
            const float mid = 0.5 * (a + c);
            const float disc = sqrt(max(0.1, mid * mid - det));
            const float lambda = mid + disc;
            const float radius = ceil(3.0 * sqrt(lambda));
            const float mx = F.fx * tx / tz + F.cx;
            const float my = F.fy * ty / tz + F.cy;
            const float tiles = float(kGsTile);
            const uint minX = uint(gsClamp((mx - radius) / tiles, 0.0, float(F.tilesX)));
            const uint minY = uint(gsClamp((my - radius) / tiles, 0.0, float(F.tilesY)));
            const uint maxX = uint(gsClamp((mx + radius + (tiles - 1.0)) / tiles, 0.0, float(F.tilesX)));
            const uint maxY = uint(gsClamp((my + radius + (tiles - 1.0)) / tiles, 0.0, float(F.tilesY)));
            culled = !(radius > 0.0) || maxX <= minX || maxY <= minY;
            if (!culled) {
                const float dx = px - F.camPos[0];
                const float dy = py - F.camPos[1];
                const float dz = pz - F.camPos[2];
                const float len = sqrt(dx * dx + dy * dy + dz * dz);
                float nx = 0.0;
                float ny = 0.0;
                float nz = 0.0;
                if (len > 0.0) {
                    nx = dx / len;
                    ny = dy / len;
                    nz = dz / len;
                }
                const float r = gsShChannel(base, 0u, F.shDegree, nx, ny, nz);
                const float g = gsShChannel(base, 1u, F.shDegree, nx, ny, nz);
                const float bl = gsShChannel(base, 2u, F.shDegree, nx, ny, nz);
                const float depth = F.depthA + F.depthB / tz;
                w0 = uint4(asuint(mx), asuint(my), asuint(tz), asuint(depth));
                w1 = uint4(asuint(c * inv), asuint(-b * inv), asuint(a * inv), asuint(s[base + 3u]));
                w2 = uint4(asuint(r), asuint(g), asuint(bl), uint(radius));
                w3 = uint4(minX, minY, maxX, maxY);
            }
        }
    }
    outp[i * 4u + 0u] = w0;
    outp[i * 4u + 1u] = w1;
    outp[i * 4u + 2u] = w2;
    outp[i * 4u + 3u] = w3;
}
