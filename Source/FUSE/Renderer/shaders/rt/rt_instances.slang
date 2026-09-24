// WP-6.0 rt_instances, Slang twin of rt_instances.comp (see there).
#include "rt_common.slang"

struct Push { // RtInstancesPush
    RtSceneInstance* instances;
    RtTransform* transforms;
    uint64_t* blasTable;
    RtAsInstance* outInstances;
    uint count;
    uint blasCount;
    uint64_t inactiveBlas;
    uint inactiveMask;
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
    const RtSceneInstance inst = pc.instances[i];
    const bool valid = (inst.flags & kFuseRtInstanceValid) != 0u;
    uint64_t blas = 0;
    if (valid && inst.mesh < pc.blasCount) {
        blas = pc.blasTable[inst.mesh];
    }
    const uint mask = fuseRtInstanceMask(inst.flags, blas);
    const RtTransform xf = pc.transforms[i];
    RtAsInstance o;
    o.rows[0] = xf.rows[0];
    o.rows[1] = xf.rows[1];
    o.rows[2] = xf.rows[2];
    // Dead slot: spec-inactive (0, 0) or, on quirk drivers, the placeholder (rt_types.hpp packRtInstance).
    const bool dead = mask == 0u;
    const bool placeholder = dead && pc.inactiveBlas != 0 && pc.inactiveMask != 0u;
    o.customIndexMask = (i & 0x00FFFFFFu) | ((dead ? (placeholder ? pc.inactiveMask : 0u) : mask) << 24u);
    o.sbtOffsetFlags = kFuseRtInstanceFlags << 24u;
    uint64_t reference = blas;
    if (dead) {
        reference = placeholder ? pc.inactiveBlas : 0;
    }
    o.blas = reference;
    pc.outInstances[i] = o;
}
