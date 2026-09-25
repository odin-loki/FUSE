// WP-8.2 "atmos.skyview". Slang twin of at_skyview.comp (see there).
#include "at_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const AtParams P = at_load(pc.params);
    const uint2 p = tid.xy;
    if (p.x >= P.skyWidth || p.y >= P.skyHeight) {
        return;
    }
    at_texels(P.skyView)[p.y * P.skyWidth + p.x] = at_sky_view_texel(P, p.x, p.y);
}
