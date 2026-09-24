// FUSE Relight RL-4.2: raster remaster vertex stage (G-buffer, decal, forward and shadow passes). Vertex pulling from
// the frame's de-indexed vertex buffer; the draw record is gl_InstanceIndex (firstInstance = the draw). Clip position
// = world x D3D VIEW x PROJECTION (the game's own camera); the shadow pass uses the frame's light matrix.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "raster_common.glsl"

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vColor;
layout(location = 4) flat out uint vDraw;

void main() {
    const RasterDraw d = RasterDrawsRef(pc.draws).v[gl_InstanceIndex];
    const RasterVertex v = RasterVerticesRef(pc.vertices).v[gl_VertexIndex];
    const vec4 world = d.objectToWorld * vec4(v.pos, 1.0);
    vWorld = world.xyz / world.w;
    vNormal = (d.normalToWorld * vec4(v.normal, 0.0)).xyz;
    vUv = v.uv;
    vColor = unpackUnorm4x8(v.color);
    vDraw = uint(gl_InstanceIndex);
    if (pc.pass == RASTER_PASS_SHADOW) {
        gl_Position = RasterFrameRef(pc.frame).f.shadowMatrix * vec4(vWorld, 1.0);
    } else {
        gl_Position = d.worldToClip * vec4(vWorld, 1.0);
    }
}
