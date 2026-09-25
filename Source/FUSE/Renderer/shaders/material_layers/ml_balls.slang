// Asset plan W0.7 "materials.balls". Slang twin of ml_balls.comp (see there).
#include "ml_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const MlParams P = ml_load(pc.params);
    const uint x = tid.x;
    const uint y = tid.y;
    if (x >= P.width || y >= P.height) {
        return;
    }
    ((float4*)P.image)[y * P.width + x] = ml_ball_pixel(P, ml_view(P), x, y);
}
