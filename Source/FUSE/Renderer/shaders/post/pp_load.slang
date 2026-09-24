// WP-4.5 "post.load". Slang twin of pp_load.comp (see there).
#include "pp_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const PpFrame F = ppFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const float4 c = ppFetch(F.inputHdr, int2(p));
    ppTexels(pc.dst)[p.y * F.width + p.x] = float4(c.xyz, 0.0);
}
