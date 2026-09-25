// WP-9.3 device-generated commands: Slang (primary) twin of dgc_generate.comp — same push layout,
// same outputs (see the GLSL file for the contract).
struct Push {
    uint64_t args;
    uint64_t drawCount;
    uint64_t scene; // GpuSceneHeader: addresses[0] = instance table
    uint64_t materialBuckets;
    uint64_t sequences;
    uint64_t bucketArgs;
    uint64_t counts;
    uint maxDraws;
    uint bucketCount;
    uint materialCount;
    uint defaultBucket;
    uint flags; // 1 = sequences, 2 = bucket args
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

static const uint kGpuInstanceWords = 8u;    // sizeof(GpuInstance) / 4
static const uint kGpuInstanceMaterial = 1u; // offsetof(GpuInstance, material) / 4

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    uint* counts = (uint*)pc.counts;
    const uint n = min(((uint*)pc.drawCount)[0], pc.maxDraws);
    if (i == 0u && (pc.flags & 1u) != 0u) {
        counts[0] = n;
    }
    if (i >= n) {
        return;
    }
    uint* args = (uint*)pc.args;
    const uint a = i * 5u;
    const uint d0 = args[a + 0u];
    const uint d1 = args[a + 1u];
    const uint d2 = args[a + 2u];
    const uint d3 = args[a + 3u];
    const uint slot = args[a + 4u];
    const uint64_t instances = ((uint64_t*)pc.scene)[0];
    const uint material = ((uint*)instances)[slot * kGpuInstanceWords + kGpuInstanceMaterial];
    uint bucket = material < pc.materialCount ? ((uint*)pc.materialBuckets)[material] : pc.defaultBucket;
    bucket = bucket < pc.bucketCount ? bucket : pc.defaultBucket;
    if ((pc.flags & 1u) != 0u) {
        uint* s = (uint*)pc.sequences;
        const uint o = i * 6u;
        s[o + 0u] = bucket;
        s[o + 1u] = d0;
        s[o + 2u] = d1;
        s[o + 3u] = d2;
        s[o + 4u] = d3;
        s[o + 5u] = slot;
    }
    if ((pc.flags & 2u) != 0u) {
        uint k = 0u;
        InterlockedAdd(counts[1u + bucket], 1u, k);
        uint* b = (uint*)pc.bucketArgs;
        const uint o = (bucket * pc.maxDraws + k) * 5u;
        b[o + 0u] = d0;
        b[o + 1u] = d1;
        b[o + 2u] = d2;
        b[o + 3u] = d3;
        b[o + 4u] = slot;
    }
}
