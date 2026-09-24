// WP-0.5 proof shader: Slang twin of shaders/compute/radix_histogram.comp (32-bit keys).
// Same bindings (set 0: keys at 0, scratch at 4), push block, workgroup size and memory order, so
// tests/test_rp_slang_twin.cpp can require bit-identical scratch buffers from both builds.
import rp_twin_common;

[[vk::push_constant]] ConstantBuffer<RadixPush> pc;
[[vk::binding(0, 0)]] StructuredBuffer<uint> keysIn;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> scratch;

groupshared uint sBins[kRadixBins];

[shader("compute")]
[numthreads(kRadixWgSize, 1, 1)]
void main(uint3 localId: SV_GroupThreadID, uint3 groupId: SV_GroupID) {
    const uint tid = localId.x;
    const uint tile = groupId.x;
    sBins[tid] = 0u;
    GroupMemoryBarrierWithGroupSync();

    const uint tileStart = tile * kRadixTileSize;
    for (uint i = 0u; i < kRadixBatchesPerTile; ++i) {
        const uint idx = tileStart + i * kRadixWgSize + tid;
        if (idx < pc.count) {
            InterlockedAdd(sBins[radixDigit(keysIn[idx], pc.shift)], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    scratch[tid * pc.numBlocks + tile] = sBins[tid];
}
