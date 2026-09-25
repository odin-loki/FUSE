#version 460
// WP-7.3 closest-hit shader of the radiance hit group (twin: pt_closest_hit.slang): the material dispatch of the SBT
// path. Builds the surface record (position from the barycentrics, geometric normal, the instance's GPU-scene
// material row, the light-tree emitter from the emitter map) with the same pt_surface the ray-query kernel calls.
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#include "lt_common.glsl"
#include "pt_common.glsl"
#include "pt_payload.glsl"

layout(location = 0) rayPayloadInEXT PtPayload payload;
hitAttributeEXT vec2 bary;

void main() {
    const PtFrame F = pt_frame();
    payload.hit = pt_surface(F, uint(gl_InstanceID), uint(gl_PrimitiveID), bary.x, bary.y, gl_HitTEXT);
}
