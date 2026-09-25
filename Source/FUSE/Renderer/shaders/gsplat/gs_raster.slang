// WP-9.2 "gsplat.raster". Slang twin of gs_raster.comp (see there).
#include "gs_common.slang"

groupshared uint4 s_w0[256];
groupshared uint4 s_w1[256];
groupshared uint4 s_w2[256];
groupshared uint s_done;

[numthreads(256, 1, 1)]
void main(uint3 ltid: SV_GroupThreadID, uint3 gid: SV_GroupID) {
    gsBegin();
    const uint tid = ltid.x;
    const uint tile = gid.x;
    const uint x = (tile % F.tilesX) * kGsTile + (tid % kGsTile);
    const uint y = (tile / F.tilesX) * kGsTile + (tid / kGsTile);
    const bool inside = x < F.width && y < F.height;
    const bool depthTest = (F.flags & kGsFlagDepthTest) != 0u;
    const bool reversed = (F.flags & kGsFlagReversedZ) != 0u;
    float scene = 1.0;
    if (inside && depthTest) {
        scene = fuse_gs_textures[F.depthHandle & 0xFFFFFu].Load(int3(int(x), int(y), 0)).x;
    }
    uint* ranges = gsUints(F.ranges);
    uint* values = gsUints(F.values);
    const uint begin = ranges[tile * 2u];
    const uint end = ranges[tile * 2u + 1u];
    const float pxc = float(x) + 0.5;
    const float pyc = float(y) + 0.5;
    float T = 1.0;
    float r = 0.0;
    float g = 0.0;
    float b = 0.0;
    bool done = !inside;
    for (uint base = begin; base < end; base += kGsGroup) {
        GroupMemoryBarrierWithGroupSync();
        if (tid == 0u) {
            s_done = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (done) {
            InterlockedAdd(s_done, 1u);
        }
        GroupMemoryBarrierWithGroupSync();
        if (s_done == kGsGroup) {
            break;
        }
        const uint e = base + tid;
        if (e < end) {
            const uint id = values[e];
            s_w0[tid] = gsProjected(id, 0u);
            s_w1[tid] = gsProjected(id, 1u);
            s_w2[tid] = gsProjected(id, 2u);
        }
        GroupMemoryBarrierWithGroupSync();
        const uint batch = min(kGsGroup, end - base);
        for (uint j = 0u; !done && j < batch; ++j) {
            const uint4 w0 = s_w0[j];
            const uint4 w1 = s_w1[j];
            const float depth = asfloat(w0.w);
            if (depthTest && !(reversed ? depth > scene : depth < scene)) {
                done = true;
                break;
            }
            const float dx = asfloat(w0.x) - pxc;
            const float dy = asfloat(w0.y) - pyc;
            const float c0 = asfloat(w1.x);
            const float c1 = asfloat(w1.y);
            const float c2 = asfloat(w1.z);
            const float power = -0.5 * (c0 * dx * dx + c2 * dy * dy) - c1 * dx * dy;
            if (power > 0.0) {
                continue;
            }
            const float alpha = min(0.99, asfloat(w1.w) * exp(power));
            if (alpha < F.alphaMin) {
                continue;
            }
            const float testT = T * (1.0 - alpha);
            if (testT < F.transmittanceMin) {
                done = true;
                break;
            }
            const uint4 w2 = s_w2[j];
            r = r + asfloat(w2.x) * alpha * T;
            g = g + asfloat(w2.y) * alpha * T;
            b = b + asfloat(w2.z) * alpha * T;
            T = testT;
        }
    }
    if (inside) {
        gsTexels(F.output_)[y * F.width + x] = float4(r, g, b, T);
    }
}
