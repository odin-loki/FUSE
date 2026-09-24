// WP-6.1 "ddgi.reset", Slang twin of ddgi_reset.comp (see there).
#include "ddgi_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    const DdgiFrame F = fuseDdgiFrame();
    const DdgiVolume V = F.volume;
    const uint irrTexels = V.probeCount * (V.irradianceRes + 2u) * (V.irradianceRes + 2u);
    const uint distTexels = V.probeCount * (V.depthRes + 2u) * (V.depthRes + 2u);
    if (i < irrTexels) {
        float* irr = (float*)V.irradiance;
        irr[i * 3u + 0u] = F.initialIrradiance[0];
        irr[i * 3u + 1u] = F.initialIrradiance[1];
        irr[i * 3u + 2u] = F.initialIrradiance[2];
    }
    if (i < distTexels) {
        float* dist = (float*)V.distance;
        dist[i * 2u + 0u] = F.maxRayDistance;
        dist[i * 2u + 1u] = F.maxRayDistance * F.maxRayDistance;
    }
    if (i < V.probeCount) {
        ((uint*)F.updateCounts)[i] = 0u;
    }
}
