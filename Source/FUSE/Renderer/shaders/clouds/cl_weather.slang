// WP-8.3 "clouds.weather". Slang twin of cl_weather.comp (see there).
#include "cl_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const CloudParams P = cl_load(pc.params);
    const uint n = P.weatherSize;
    if (tid.x >= n || tid.y >= n) {
        return;
    }
    cl_texels(P.weather)[tid.y * n + tid.x] = cl_weather_texel(P, tid.x, tid.y);
}
