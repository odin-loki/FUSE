// WP-7.3 closest-hit shader of the radiance hit group, Slang twin of pt_closest_hit.rchit (see there).
#include "lt_common.slang"
#include "pt_common.slang"

[shader("closesthit")]
void main(inout PtPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    const PtFrame F = ptFrame();
    payload.hit = ptSurface(F, InstanceIndex(), PrimitiveIndex(), attr.barycentrics.x, attr.barycentrics.y, RayTCurrent());
}
