// WP-8.3 "clouds.reconstruct". Slang twin of cl_reconstruct.comp (see there).
#include "cl_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const CloudParams P = cl_load(pc.params);
    const uint x = tid.x;
    const uint y = tid.y;
    if (x >= P.width || y >= P.height) {
        return;
    }
    float4 o0;
    float4 o1;
    cl_reconstruct_texel(P, x, y, o0, o1);
    float4* out_ = cl_texels(P.historyOut);
    out_[(y * P.width + x) * 2u] = o0;
    out_[(y * P.width + x) * 2u + 1u] = o1;
}
