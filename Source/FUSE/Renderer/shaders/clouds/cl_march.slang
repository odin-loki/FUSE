// WP-8.3 "clouds.march". Slang twin of cl_march.comp (see there).
#include "cl_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const CloudParams P = cl_load(pc.params);
    const uint fx = tid.x;
    const uint fy = tid.y;
    if (fx >= P.freshWidth || fy >= P.freshHeight) {
        return;
    }
    float4 o0;
    float4 o1;
    cl_march_texel(P, fx, fy, o0, o1);
    float4* out_ = cl_texels(P.fresh);
    out_[(fy * P.freshWidth + fx) * 2u] = o0;
    out_[(fy * P.freshWidth + fx) * 2u + 1u] = o1;
}
