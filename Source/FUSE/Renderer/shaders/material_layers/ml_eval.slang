// Asset plan W0.7 "materials.eval". Slang twin of ml_eval.comp (see there).
#include "ml_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    const MlParams P = ml_load(pc.params);
    const MlSurface s = ((MlSurface*)pc.src)[i];
    ((MlResult*)pc.dst)[i] = ml_evaluate(ml_view(P), s);
}
