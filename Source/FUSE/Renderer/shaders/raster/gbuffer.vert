#version 450
// B5.2 raster G-buffer pass (GBufferRasterPass). Per-draw surface data arrives as push constants;
// the draw's NDC depth is surface.z so overlapping draws exercise the depth test.
layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform GBufferPush {
    vec4 albedo;   // rgb albedo, a unused
    vec4 normal;   // xyz world normal
    vec4 surface;  // x roughness, y metallic, z NDC depth, w ambient occlusion
    vec4 emissive; // xyz emissive radiance, w shading model
} pc;

void main() {
    gl_Position = vec4(inPosition.xy, pc.surface.z, 1.0);
}
