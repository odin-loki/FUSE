// WP-6.1 "ddgi.raygen", Slang twin of ddgi_raygen.comp (see there).
#include "ddgi_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint r = tid.x;
    const DdgiFrame F = fuseDdgiFrame();
    const uint n = F.raysPerProbe;
    if (r >= n) {
        return;
    }
    const uint phase = r * 0x61C88647u;
    const float phi = float(phase) * float(6.28318530717958647692 / 4294967296.0);
    const float z = 1.0 - (2.0 * float(r) + 1.0) / float(n);
    const float rr = sqrt(max(0.0, 1.0 - z * z));
    const float3 v = float3(rr * cos(phi), rr * sin(phi), z);
    const float3 d = float3(F.rotation[0].x * v.x + F.rotation[0].y * v.y + F.rotation[0].z * v.z,
                            F.rotation[1].x * v.x + F.rotation[1].y * v.y + F.rotation[1].z * v.z,
                            F.rotation[2].x * v.x + F.rotation[2].y * v.y + F.rotation[2].z * v.z);
    ((float4*)F.rayDirs)[r] = float4(fuseDdgiNormalized(d), 0.0);
}
