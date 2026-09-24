// WP-6.0 rt_probe, Slang twin of rt_probe.comp (see there).
#include "rt_common.slang"

struct Push { // RtProbePush
    uint64_t tlas;
    RtRay* rays;
    RtHit* hits;
    uint count;
    uint cullMask;
    uint rayFlags;
    uint pad0;
    uint pad1;
    uint pad2;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    const RtRay ray = pc.rays[i];
    RayDesc desc;
    desc.Origin = ray.originTMin.xyz;
    desc.TMin = ray.originTMin.w;
    desc.Direction = ray.directionTMax.xyz;
    desc.TMax = ray.directionTMax.w;
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(pc.tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, pc.rayFlags | RAY_FLAG_FORCE_OPAQUE, pc.cullMask & 0x7Fu, desc); // bit 7 = kRtMaskDead, never traced
    while (q.Proceed()) {
    }
    RtHit h;
    h.t = -1.0;
    h.instance = kFuseRtInvalid;
    h.customIndex = kFuseRtInvalid;
    h.primitive = kFuseRtInvalid;
    h.u = 0.0;
    h.v = 0.0;
    h.flags = 0u;
    h.geometry = kFuseRtInvalid;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        const float2 bary = q.CommittedTriangleBarycentrics();
        h.t = q.CommittedRayT();
        h.instance = q.CommittedInstanceIndex();
        h.customIndex = q.CommittedInstanceID();
        h.primitive = q.CommittedPrimitiveIndex();
        h.geometry = q.CommittedGeometryIndex();
        h.u = bary.x;
        h.v = bary.y;
        h.flags = kFuseRtHit | (q.CommittedTriangleFrontFace() ? kFuseRtFrontFace : 0u);
    }
    pc.hits[i] = h;
}
