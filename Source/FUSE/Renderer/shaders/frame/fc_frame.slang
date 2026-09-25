// Frame composer kernel. Slang twin of fc_frame.comp (see there).
#include "fc_common.slang"

[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_fc_images[];

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    fcBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint i = p.y * F.width + p.x;
    const uint mode = pc.mode;
    if (mode == kFcModeShadowPack) {
        fcFloats(F.visibility)[i] = min(1.0, max(0.0, fcTexels(F.denoised)[i].x));
        return;
    }
    if (mode == kFcModeResolve) {
        float4 base;
        if ((F.flags & kFcFlagClouds) != 0u) {
            base = fcTexels(F.clouds)[i];
        } else {
            base = fcFetch(F.inColor, int2(p));
        }
        float r = base.x;
        float g = base.y;
        float b = base.z;
        if ((F.flags & kFcFlagSplats) != 0u) {
            const float4 s = fcTexels(F.splats)[i];
            r = r * s.w + s.x;
            g = g * s.w + s.y;
            b = b * s.w + s.z;
        }
        fuse_fc_images[F.outResolve & 0xFFFFFu][p] = float4(r, g, b, 1.0);
        return;
    }
    const float4 c = fcFetch(F.inColor, int2(p));
    const float depth = fcFetch(F.inDepth, int2(p)).x;
    if (mode == kFcModeGather) {
        fcTexels(F.background)[i] = c;
        fcFloats(F.distance)[i] = fcViewDistance(p, depth);
        return;
    }
    float4 result = c;
    if (!(depth < 1.0)) {
        if ((F.flags & kFcFlagSky) != 0u) {
            const AtParams atm = at_load(F.atmosphere);
            const float3 far = fcUnproject(p, 1.0);
            const float3 dir = normalize(float3(far.x - F.cameraPos[0], far.y - F.cameraPos[1], far.z - F.cameraPos[2]));
            result = float4(at_sky_radiance(atm, dir, (F.flags & kFcFlagSunDisk) != 0u), 1.0);
        }
    } else if ((F.flags & kFcFlagAerial) != 0u) {
        const AtParams atm = at_load(F.atmosphere);
        const float2 uv = float2((float(p.x) + 0.5) / float(F.width), (float(p.y) + 0.5) / float(F.height));
        float3 s;
        float3 t;
        at_aerial(atm, uv, fcViewDistance(p, depth), s, t);
        const float3 e = at_illuminance(atm);
        result = float4(c.xyz * t + s * e, c.w);
    }
    fuse_fc_images[F.outSky & 0xFFFFFu][p] = result;
}
