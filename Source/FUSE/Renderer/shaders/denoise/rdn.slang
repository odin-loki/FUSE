// RL-5.5 radiance denoiser: every pass of the single-source core (rdn_core.h has the algorithm), selected by
// pc.pass. Primary kernel (compile with -fp-mode precise); GLSL twin: rdn.comp; CPU reference: rdn_kernel.hpp.
struct RdnPush {
    uint64_t frame;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
    uint pass;
    uint stp;
};
[[vk::push_constant]] ConstantBuffer<RdnPush> pc;

#define FUSE_RDN_FN
#define FUSE_RDN_IN(T) T
#define FUSE_RDN_INOUT(T) inout T

float4 rdnLd4(uint64_t a, uint i) { return ((float4*)a)[i]; }
void rdnSt4(uint64_t a, uint i, float4 v) { ((float4*)a)[i] = v; }
float2 rdnLd2(uint64_t a, uint i) { return ((float2*)a)[i]; }
float rdnLd1(uint64_t a, uint i) { return ((float*)a)[i]; }
uint rdnLdU(uint64_t a, uint i) { return ((uint*)a)[i]; }

#include "rdn_core.h"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    RdnFrame F = ((RdnFrame*)pc.frame)[0];
    rdnRun(F, pc.pass, pc.stp, pc.a0, pc.a1, pc.a2, pc.a3, pc.a4, pc.a5, tid.x, tid.y);
}
