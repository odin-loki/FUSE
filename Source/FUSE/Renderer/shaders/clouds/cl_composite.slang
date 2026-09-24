// WP-8.3 "clouds.composite". Slang twin of cl_composite.comp (see there).
#include "cl_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const CloudParams P = cl_load(pc.params);
    if (tid.x >= P.outWidth || tid.y >= P.outHeight) {
        return;
    }
    cl_texels(P.result)[tid.y * P.outWidth + tid.x] = cl_composite_texel(P, tid.x, tid.y);
}
