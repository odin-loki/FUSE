#version 460
// WP-7.3 miss shader (twin: pt_miss.slang), both ray types: t = -1 (radiance: the ray escaped to the sky; shadow:
// the segment is visible).
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#include "lt_common.glsl"
#include "pt_common.glsl"
#include "pt_payload.glsl"

layout(location = 0) rayPayloadInEXT PtPayload payload;

void main() {
    payload.hit.t = -1.0;
}
