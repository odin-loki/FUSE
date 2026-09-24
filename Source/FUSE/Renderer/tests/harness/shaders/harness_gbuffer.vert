#version 450
// WP-0.7 renderer test harness: vertex stage for GBufferRasterPass with CPU-projected geometry.
// Identical interface to shaders/raster/gbuffer.vert (location 0 vec3, 64-byte GBufferPush), but the
// NDC depth comes from the vertex (the harness projects scenes on the CPU) instead of pc.surface.z.
// The fragment stage is the stock gbuffer.frag, so write_gbuffer() packing is unchanged.
layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform GBufferPush {
    vec4 albedo;
    vec4 normal;
    vec4 surface;
    vec4 emissive;
} pc;

void main() {
    gl_Position = vec4(inPosition, 1.0);
}
