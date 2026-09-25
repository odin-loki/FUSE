// "ssrc.prepare". Slang twin of src_prepare.comp (see there).
#include "src_common.slang"

[[vk::binding(1, 0)]] Texture2D<float4> fuse_src_textures[];

float4 srcFetch(uint handle, int2 p) { return fuse_src_textures[handle & 0xFFFFFu].Load(int3(p, 0)); }

float srcLinearDepth(float d) {
    float z = 0.0;
    if ((F.flags & kSrcFlagReversedZ) != 0u) {
        if (d > 0.0) {
            z = F.nearPlane / d;
        }
    } else if (d < 1.0 && F.farPlane > F.nearPlane) {
        z = (F.nearPlane * F.farPlane) / (F.farPlane - d * (F.farPlane - F.nearPlane));
    }
    return (z > 0.0 && z < 3.0e38) ? z : 0.0;
}

float3 srcOctDecode(float ox, float oy) {
    float3 n = float3(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        const float x = (1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return srcNormalize(n);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    srcBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const int2 ip = int2(p);
    const float rt4 = srcFetch(F.inputDepth, ip).x;
    const float4 rt0 = srcFetch(F.inputNormal, ip);
    const float4 rt1 = srcFetch(F.inputAlbedo, ip);
    const float4 rt2 = srcFetch(F.inputRoughMetal, ip);
    const float4 lit = srcFetch(F.inputLit, ip);
    const uint i = p.y * F.width + p.x;

    const float z = srcLinearDepth(rt4);
    const float3 w = srcOctDecode(rt0.x, rt0.y);
    const float vx = F.viewRot[0] * w.x + F.viewRot[4] * w.y + F.viewRot[8] * w.z;
    const float vy = F.viewRot[1] * w.x + F.viewRot[5] * w.y + F.viewRot[9] * w.z;
    const float vz = F.viewRot[2] * w.x + F.viewRot[6] * w.y + F.viewRot[10] * w.z;
    const float dw = 1.0 - rt2.y;

    srcTexels(F.geo)[i] = float4(z, vx, -vy, -vz);
    srcTexels(F.lit)[i] = lit;
    srcTexels(F.diffuse)[i] = float4(rt1.x * dw, rt1.y * dw, rt1.z * dw, rt0.w);
    srcTexels(F.albedo)[i] = float4(rt1.xyz, 0.0);
}
