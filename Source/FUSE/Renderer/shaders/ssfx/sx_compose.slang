// WP-6.3 "ssfx.compose". Slang twin of sx_compose.comp (see there).
#include "sx_common.slang"

[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_sx_images[];

float sxMultiBounce(float visibility, float albedo) {
    const float a = 2.04040003 * albedo - 0.332399994;
    const float b = -4.79510021 * albedo + 0.641700029;
    const float c = 2.75519991 * albedo + 0.690299988;
    return max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    sxBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint i = sxIndex(p.x, p.y);
    const float4 prep = sxTexels(F.prepared)[i];
    const float4 lit = sxTexels(F.radiance)[i];
    const float3 albedo = sxTexels(F.albedo)[i].xyz;
    float r = lit.x;
    float g = lit.y;
    float b = lit.z;
    if ((F.flags & kSxFlagAo) != 0u) {
        const float ao = sxFloats(F.ao)[i];
        float vr = ao;
        float vg = ao;
        float vb = ao;
        if ((F.flags & kSxFlagMultiBounce) != 0u) {
            vr = sxMultiBounce(ao, albedo.x);
            vg = sxMultiBounce(ao, albedo.y);
            vb = sxMultiBounce(ao, albedo.z);
        }
        const float k = prep.w;
        r = r + F.ambient[0] * albedo.x * k * (vr - 1.0);
        g = g + F.ambient[1] * albedo.y * k * (vg - 1.0);
        b = b + F.ambient[2] * albedo.z * k * (vb - 1.0);
    }
    if ((F.flags & kSxFlagSsr) != 0u) {
        const float4 ssr = sxTexels(F.ssr)[i];
        if (prep.x > 0.0 && ssr.w > 0.0) {
            const float3 q = sxUnproject(float(p.x) + 0.5, float(p.y) + 0.5, prep.x);
            float3 n = sxNormalize(sxTexels(F.normals)[i].xyz);
            if (sxDot(n, q) > 0.0) {
                n = sxNeg(n);
            }
            const float3 v = sxNormalize(sxNeg(q));
            const float nov = max(0.0, min(1.0, sxDot(n, v)));
            const float m = 1.0 - nov;
            const float m5 = m * m * m * m * m;
            const float metal = prep.z;
            const float dielectric = 0.04 * (1.0 - metal);
            const float f0r = dielectric + albedo.x * metal;
            const float f0g = dielectric + albedo.y * metal;
            const float f0b = dielectric + albedo.z * metal;
            r = r + ssr.x * ssr.w * (f0r + (1.0 - f0r) * m5);
            g = g + ssr.y * ssr.w * (f0g + (1.0 - f0g) * m5);
            b = b + ssr.z * ssr.w * (f0b + (1.0 - f0b) * m5);
        }
    }
    if ((F.flags & kSxFlagSsgi) != 0u) {
        const float3 gi = sxTexels(F.gi)[i].xyz;
        r = r + gi.x;
        g = g + gi.y;
        b = b + gi.z;
    }
    const float4 result = float4(max(r, 0.0), max(g, 0.0), max(b, 0.0), lit.w);
    fuse_sx_images[F.output_ & 0xFFFFFu][p] = result;
    if (F.dump != 0) {
        sxTexels(F.dump)[i] = result;
    }
}
