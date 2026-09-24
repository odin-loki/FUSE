// WP-7.2 "restir.prepare", Slang twin of restir_prepare.comp (see there). Compile with -fp-mode precise.
#include "gpu_scene.slang"
#include "lt_common.slang"
#include "restir_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const RestirFrame F = rsFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint pixel = p.y * F.width + p.x;
    const float depth = rsFetch(F.gbufferDepth, int2(p)).x;
    const float4 rt0 = rsFetch(F.gbufferNormal, int2(p));
    const float4 rt1 = rsFetch(F.gbufferAlbedo, int2(p));
    RsSurface s;
    const bool ok = rsPrepare(F, p.x, p.y, depth, rt0.x, rt0.y, s);
    ((float4*)F.surfPos[0])[pixel] = ok ? float4(s.p, s.depth) : float4(0.0, 0.0, 0.0, 0.0);
    ((float4*)F.surfNormal[0])[pixel] = ok ? float4(s.n, 0.0) : float4(0.0, 0.0, 0.0, 0.0);
    ((float4*)F.surfAlbedo[0])[pixel] = ok ? float4(rt1.xyz, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
}
