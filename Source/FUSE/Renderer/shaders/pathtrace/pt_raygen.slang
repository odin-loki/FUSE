// WP-7.3 ray-generation shader (tier T3), Slang twin of pt_raygen.rgen (see there).
#include "lt_common.slang"
#include "pt_common.slang"

static PtPayload g_payload;

bool ptTrace(PtFrame F, float3 o, float3 d, float tMin, float tMax, uint originInstance, uint originPrimitive, out PtHit hit) {
    RayDesc desc;
    desc.Origin = o;
    desc.TMin = tMin;
    desc.Direction = d;
    desc.TMax = tMax;
    PtPayload payload;
    payload.hit = ptMissRecord();
    payload.originInstance = originInstance;
    payload.originPrimitive = originPrimitive;
    payload.pad0 = 0u;
    payload.pad1 = 0u;
    TraceRay(RaytracingAccelerationStructure(F.tlas), RAY_FLAG_FORCE_NON_OPAQUE, F.cullMask & kPtMaskAll, kPtRayRadiance, kPtRayTypes, 0,
             desc, payload);
    hit = payload.hit;
    return hit.t >= 0.0;
}

bool ptOccluded(PtFrame F, float3 o, float3 d, float tMax, uint originInstance, uint originPrimitive) {
    RayDesc desc;
    desc.Origin = o;
    desc.TMin = F.rayTMin;
    desc.Direction = d;
    desc.TMax = tMax;
    PtPayload payload;
    payload.hit = ptMissRecord();
    payload.hit.t = 0.0; // stays 0 when a hit ends the ray; the miss shader writes -1
    payload.originInstance = originInstance;
    payload.originPrimitive = originPrimitive;
    payload.pad0 = 0u;
    payload.pad1 = 0u;
    TraceRay(RaytracingAccelerationStructure(F.tlas),
             RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             F.cullMask & kPtMaskAll, kPtRayShadow, kPtRayTypes, 0, desc, payload);
    return payload.hit.t >= 0.0;
}

#include "pt_integrator.slang"

[shader("raygeneration")]
void main() {
    const PtFrame F = ptFrame();
    const uint2 p = DispatchRaysIndex().xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    ptRenderPixel(F, p.x, p.y);
}
