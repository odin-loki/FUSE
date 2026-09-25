// WP-7.2 "restir.di.temporal / restir.di.spatial", Slang twin of restir_di_reuse.comp (see there). Compile with -fp-mode precise.
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
    ((RestirDiReservoir*)pc.dst)[pixel] = rsDiReuse(F, p.x, p.y, pc.mode, pc.iteration);
}
