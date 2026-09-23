#version 450
#extension GL_GOOGLE_include_directive : require
// B5.2 raster G-buffer pass: MRT outputs in GBufferAttachment order (RT0..RT5), packed by the
// shared write_gbuffer() helper that GBufferPacking mirrors on the CPU. RT4 is the R32F depth copy.
#include "gbuffer.glsl"

layout(location = 0) out vec4 outNormalAo;
layout(location = 1) out vec4 outAlbedoAlpha;
layout(location = 2) out vec4 outRoughMetal;
layout(location = 3) out vec4 outVelocity;
layout(location = 4) out vec4 outDepth;
layout(location = 5) out vec4 outEmissive;

layout(push_constant) uniform GBufferPush {
    vec4 albedo;
    vec4 normal;
    vec4 surface;
    vec4 emissive;
} pc;

void main() {
    vec4 rt0;
    vec4 rt1;
    vec4 rt2;
    vec4 rt3;
    vec4 rt5;
    write_gbuffer(pc.normal.xyz, pc.albedo.rgb, pc.surface.x, pc.surface.y, pc.emissive.xyz, pc.surface.w,
                  vec2(0.0), uint(pc.emissive.w), rt0, rt1, rt2, rt3, rt5);
    outNormalAo = rt0;
    outAlbedoAlpha = rt1;
    outRoughMetal = rt2;
    outVelocity = rt3;
    outDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);
    outEmissive = rt5;
}
