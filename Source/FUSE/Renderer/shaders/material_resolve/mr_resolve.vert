#version 460
// WP-1.5 material resolve, vertex stage. Specialisation constant 0 = the bin:
//   0..3  binned path: one instance per tile of that bin (vkCmdDrawIndirect, instanceCount = the
//         classify pass's tile count), 6 vertices = the tile's quad, from the bin's tile list (BDA)
//   4     uber path: one full-screen triangle (vkCmdDraw(3))
// GLSL twin of mr_resolve_vs.slang.
#extension GL_GOOGLE_include_directive : require
#include "mr_includes.glsl"

layout(constant_id = 0) const uint FUSE_MR_BIN_ID = 4u;

void main() {
    const uint i = uint(gl_VertexIndex);
    if (FUSE_MR_BIN_ID >= FUSE_MR_BIN_UBER) {
        gl_Position = vec4(float((i << 1u) & 2u) * 2.0 - 1.0, float(i & 2u) * 2.0 - 1.0, 0.0, 1.0);
        return;
    }
    const FuseMrFrame f = FuseMrFrameRef(pc.frame).f;
    const uint tile = FuseMrBinsRef(f.bins).words[FUSE_MR_BIN_LIST_OFFSET_WORDS + FUSE_MR_BIN_ID * f.tileCapacity +
                                                  uint(gl_InstanceIndex)];
    const uint cx = (i == 1u || i == 4u || i == 5u) ? 1u : 0u;
    const uint cy = (i == 2u || i == 3u || i == 5u) ? 1u : 0u;
    const float px = float(((tile & 0xFFFFu) + cx) * FUSE_MR_TILE);
    const float py = float(((tile >> 16u) + cy) * FUSE_MR_TILE);
    gl_Position = vec4(px / float(f.width) * 2.0 - 1.0, py / float(f.height) * 2.0 - 1.0, 0.0, 1.0);
}
