// WP-6.3 "ssfx.prepare". Slang twin of sx_prepare.comp (see there).
#include "sx_common.slang"

float sxLinearDepth(float d) {
    float z = 0.0;
    if ((F.flags & kSxFlagReversedZ) != 0u) {
        if (d > 0.0) {
            z = F.nearPlane / d;
        }
    } else if (d < 1.0 && F.farPlane > F.nearPlane) {
        z = (F.nearPlane * F.farPlane) / (F.farPlane - d * (F.farPlane - F.nearPlane));
    }
    return (z > 0.0 && z < 3.0e38) ? z : 0.0;
}

float3 sxOctDecode(float ox, float oy) {
    float3 n = float3(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        const float x = (1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return sxNormalize(n);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    sxBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const int2 ip = int2(p);
    const float rt4 = sxFetch(F.inputDepth, ip).x;
    const float4 rt0 = sxFetch(F.inputNormal, ip);
    const float4 rt1 = sxFetch(F.inputAlbedo, ip);
    const float4 rt2 = sxFetch(F.inputRoughMetal, ip);
    const float4 lit = sxFetch(F.inputLit, ip);
    const uint i = sxIndex(p.x, p.y);

    const float z = sxLinearDepth(rt4);
    const float3 w = sxOctDecode(rt0.x, rt0.y);
    const float vx = F.viewRot[0] * w.x + F.viewRot[4] * w.y + F.viewRot[8] * w.z;
    const float vy = F.viewRot[1] * w.x + F.viewRot[5] * w.y + F.viewRot[9] * w.z;
    const float vz = F.viewRot[2] * w.x + F.viewRot[6] * w.y + F.viewRot[10] * w.z;
    const float dw = 1.0 - rt2.y;

    sxTexels(F.prepared)[i] = float4(z, rt2.x, rt2.y, rt0.w);
    sxTexels(F.normals)[i] = float4(vx, -vy, -vz, 0.0);
    sxTexels(F.radiance)[i] = lit;
    sxTexels(F.albedo)[i] = float4(rt1.xyz, 0.0);
    sxTexels(F.diffuse)[i] = float4(rt1.x * dw, rt1.y * dw, rt1.z * dw, 0.0);
}
