// WP-7.2 "restir.shade", Slang twin of restir_shade.comp (see there). Compile with -fp-mode precise.
#include "gpu_scene.slang"
#include "lt_common.slang"
#include "restir_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const RestirFrame F = rsFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint pixel = p.y * F.width + p.x;
    const bool hasDi = pc.src != 0ull;
    const bool hasGi = pc.aux != 0ull;
    const RestirDiReservoir di = hasDi ? ((RestirDiReservoir*)pc.src)[pixel] : rsDiEmpty();
    const RestirGiReservoir gi = hasGi ? ((RestirGiReservoir*)pc.aux)[pixel] : rsGiEmpty();
    float4 diOut;
    float4 giOut;
    float depth;
    rsShade(F, p.x, p.y, hasDi, di, hasGi, gi, diOut, giOut, depth);
    ((float4*)F.diSignal)[pixel] = diOut;
    ((float4*)F.giSignal)[pixel] = giOut;
    ((float*)F.depth)[pixel] = depth;
    ((RestirDiReservoir*)F.diHistory[0])[pixel] = di;
    ((RestirGiReservoir*)F.giHistory[0])[pixel] = gi;
}
