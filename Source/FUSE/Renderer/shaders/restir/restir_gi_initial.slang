// WP-7.2 "restir.gi.initial", Slang twin of restir_gi_initial.comp (see there). Compile with -fp-mode precise.
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
    ((RestirGiReservoir*)pc.dst)[pixel] = rsGiInitial(F, p.x, p.y, pc.aux);
}
