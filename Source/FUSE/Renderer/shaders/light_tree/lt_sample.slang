// WP-7.1 "light_tree.sample". Slang twin of lt_sample.comp (see there); compiled with -fp-mode precise.
#include "lt_common.slang"

struct LtPush {
    uint64_t header;
    uint64_t queries;
    uint64_t results;
    uint count;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<LtPush> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    const LtQuery q = ((LtQuery*)pc.queries)[i];
    const float3 p = float3(q.position[0], q.position[1], q.position[2]);
    const float3 n = float3(q.normal[0], q.normal[1], q.normal[2]);
    LtSampleResult s = ltSample(pc.header, p, n, q.u0, q.u1, q.u2);
    s.evalPmf = q.evalLight != kLtInvalid ? ltPmf(pc.header, p, n, q.evalLight) : 0.0;
    ((LtSampleResult*)pc.results)[i] = s;
}
