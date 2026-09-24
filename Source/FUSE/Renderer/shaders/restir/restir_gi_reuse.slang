// WP-7.2 "restir.gi.temporal / restir.gi.spatial", Slang twin of restir_gi_reuse.comp (see there). Compile with -fp-mode precise.
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
    ((RestirGiReservoir*)pc.dst)[pixel] = rsGiReuse(F, p.x, p.y, pc.mode, pc.iteration);
}
