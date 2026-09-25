// WP-7.3 "pathtrace.trace" on ray queries (tier T2), Slang twin of pt_trace.comp (see there).
#include "lt_common.slang"
#include "pt_common.slang"

bool ptTrace(PtFrame F, float3 o, float3 d, float tMin, float tMax, uint originInstance, uint originPrimitive, out PtHit hit) {
    RayDesc desc;
    desc.Origin = o;
    desc.TMin = tMin;
    desc.Direction = d;
    desc.TMax = tMax;
    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(F.tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(as, RAY_FLAG_FORCE_NON_OPAQUE, F.cullMask & kPtMaskAll, desc);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            if (q.CandidateInstanceIndex() != originInstance || q.CandidatePrimitiveIndex() != originPrimitive) {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        hit = ptMissRecord();
        return false;
    }
    const float2 bary = q.CommittedTriangleBarycentrics();
    hit = ptSurface(F, q.CommittedInstanceIndex(), q.CommittedPrimitiveIndex(), bary.x, bary.y, q.CommittedRayT());
    return true;
}

bool ptOccluded(PtFrame F, float3 o, float3 d, float tMax, uint originInstance, uint originPrimitive) {
    RayDesc desc;
    desc.Origin = o;
    desc.TMin = F.rayTMin;
    desc.Direction = d;
    desc.TMax = tMax;
    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(F.tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(as, RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, F.cullMask & kPtMaskAll, desc);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            if (q.CandidateInstanceIndex() != originInstance || q.CandidatePrimitiveIndex() != originPrimitive) {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#include "pt_integrator.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const PtFrame F = ptFrame();
    if (tid.x >= F.width || tid.y >= F.height) {
        return;
    }
    ptRenderPixel(F, tid.x, tid.y);
}
