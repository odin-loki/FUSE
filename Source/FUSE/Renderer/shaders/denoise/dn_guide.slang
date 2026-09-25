// WP-6.4 "denoise.guide". Slang twin of dn_guide.comp (see there).
#include "dn_common.slang"

float dnDepthSlope(float z, float zm, bool hasM, float zp, bool hasP) {
    const float dm = z - zm;
    const float dp = zp - z;
    if (hasM && hasP) {
        return abs(dm) <= abs(dp) ? dm : dp;
    }
    if (hasM) {
        return dm;
    }
    if (hasP) {
        return dp;
    }
    return 0.0;
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const DnFrame F = dnFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint w = F.width;
    const uint i = p.y * w + p.x;
    float* depth = dnFloats(F.depth);
    const float z = depth[i];
    if (!(z > 0.0)) {
        dnTexels(F.guideCur)[i] = float4(0.0, 0.0, 0.0, 0.0);
        dnPairs(F.gradZ)[i] = float2(0.0, 0.0);
        return;
    }
    float4 n;
    if ((F.flags & kDnFlagNormalImage) != 0u) {
        n = fuse_dn_textures[F.normalImage & 0xFFFFFu].Load(int3(int2(p), 0));
    } else {
        n = dnTexels(F.normal)[i];
    }
    if ((F.flags & kDnFlagNormalOct) != 0u) {
        n = dnOctDecode(n.x, n.y);
    }
    n.w = z;
    const float zl = p.x > 0u ? depth[i - 1u] : 0.0;
    const float zr = p.x + 1u < w ? depth[i + 1u] : 0.0;
    const float zu = p.y > 0u ? depth[i - w] : 0.0;
    const float zd = p.y + 1u < F.height ? depth[i + w] : 0.0;
    dnTexels(F.guideCur)[i] = n;
    dnPairs(F.gradZ)[i] = float2(dnDepthSlope(z, zl, zl > 0.0, zr, zr > 0.0), dnDepthSlope(z, zu, zu > 0.0, zd, zd > 0.0));
}
