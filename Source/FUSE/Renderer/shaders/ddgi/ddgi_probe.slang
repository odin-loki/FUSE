// WP-6.1 "ddgi.probe", Slang twin of ddgi_probe.comp (see there).
#include "ddgi_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    const DdgiPoint p = ((DdgiPoint*)pc.aux)[i];
    const float3 e = fuseDdgiSampleIrradiance(pc.frame, p.position.xyz, p.normal.xyz);
    ((float4*)pc.out_)[i] = float4(e, 1.0);
}
