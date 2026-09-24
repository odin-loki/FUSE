// WP-8.3 "clouds.noise.detail". Slang twin of cl_noise_detail.comp (see there).
#include "cl_common.slang"

[numthreads(4, 4, 4)]
void main(uint3 tid: SV_DispatchThreadID) {
    const CloudParams P = cl_load(pc.params);
    const uint n = P.detailSize;
    if (tid.x >= n || tid.y >= n || tid.z >= n) {
        return;
    }
    cl_texels(P.detailNoise)[(tid.z * n + tid.y) * n + tid.x] = cl_detail_texel(P, tid);
}
