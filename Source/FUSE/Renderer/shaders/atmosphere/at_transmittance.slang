// WP-8.2 "atmos.transmittance". Slang twin of at_transmittance.comp (see there).
#include "at_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const AtParams P = at_load(pc.params);
    const uint2 p = tid.xy;
    if (p.x >= P.transWidth || p.y >= P.transHeight) {
        return;
    }
    at_texels(P.transmittance)[p.y * P.transWidth + p.x] = at_transmittance_texel(P, p.x, p.y);
}
