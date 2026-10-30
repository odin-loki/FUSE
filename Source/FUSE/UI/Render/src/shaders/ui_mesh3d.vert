#version 450
// ui.mesh3d vertex stage: the GPU half of fuse/ui/mesh3d.hpp (rasterMesh is the CPU specification).
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 light; // xyz: unit direction to the light (model space), w: ambient
    vec4 size;  // xy: target size in pixels
} pc;
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor; // R8G8B8A8_UNORM, straight
layout(location = 3) in uint inId;
layout(location = 0) noperspective out vec4 vColor;
layout(location = 1) flat out uint vId; // provoking (first) vertex of the triangle, as rasterMesh
void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    float l = length(inNormal);
    float d = l > 0.0 ? abs(dot(inNormal, pc.light.xyz) / l) : 1.0;
    float k = pc.light.w + (1.0 - pc.light.w) * min(d, 1.0);
    vColor = vec4(inColor.rgb * k, inColor.a);
    vId = inId;
}
