// WP-7.3 any-hit shader of both hit groups, Slang twin of pt_any_hit.rahit (see there).
#include "lt_common.slang"
#include "pt_common.slang"

[shader("anyhit")]
void main(inout PtPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    if (InstanceIndex() == payload.originInstance && PrimitiveIndex() == payload.originPrimitive) {
        IgnoreHit();
    }
}
