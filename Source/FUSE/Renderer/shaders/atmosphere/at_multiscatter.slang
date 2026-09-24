// WP-8.2 "atmos.multiscatter". Slang twin of at_multiscatter.comp (see there).
#include "at_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const AtParams P = at_load(pc.params);
    const uint2 p = tid.xy;
    if (p.x >= P.msWidth || p.y >= P.msHeight) {
        return;
    }
    at_texels(P.multiscatter)[p.y * P.msWidth + p.x] = at_multiscatter_texel(P, p.x, p.y);
}
