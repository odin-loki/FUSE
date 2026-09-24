// WP-8.3 "clouds.noise.shape". Slang twin of cl_noise_shape.comp (see there).
#include "cl_common.slang"

[numthreads(4, 4, 4)]
void main(uint3 tid: SV_DispatchThreadID) {
    const CloudParams P = cl_load(pc.params);
    const uint n = P.shapeSize;
    if (tid.x >= n || tid.y >= n || tid.z >= n) {
        return;
    }
    cl_texels(P.shapeNoise)[(tid.z * n + tid.y) * n + tid.x] = cl_shape_texel(P, tid);
}
