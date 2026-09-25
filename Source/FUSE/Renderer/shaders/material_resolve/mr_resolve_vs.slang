// WP-1.5 material resolve, vertex stage. Slang twin of mr_resolve.vert (see there). The draws use
// firstVertex = firstInstance = 0, so SV_VertexID / SV_InstanceID equal gl_VertexIndex / gl_InstanceIndex.
#include "mr_includes.slang"

[[vk::constant_id(0)]] const uint kFuseMrBinId = 4u;

[shader("vertex")]
float4 main(uint i : SV_VertexID, uint instance : SV_InstanceID) : SV_Position {
    if (kFuseMrBinId == kFuseMrBinUber) {
        return float4(float((i << 1u) & 2u) * 2.0 - 1.0, float(i & 2u) * 2.0 - 1.0, 0.0, 1.0);
    }
    const MrFrame f = fuseMrFrame();
    uint* words = (uint*)f.bins;
    const uint tile = words[fuseMrBinListWord(kFuseMrBinId, f.tileCapacity) + instance];
    const uint cx = (i == 1u || i == 4u || i == 5u) ? 1u : 0u;
    const uint cy = (i == 2u || i == 3u || i == 5u) ? 1u : 0u;
    const float px = float(((tile & 0xFFFFu) + cx) * kFuseMrTile);
    const float py = float(((tile >> 16u) + cy) * kFuseMrTile);
    return float4(px / float(f.width) * 2.0 - 1.0, py / float(f.height) * 2.0 - 1.0, 0.0, 1.0);
}
