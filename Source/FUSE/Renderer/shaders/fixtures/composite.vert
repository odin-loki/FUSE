#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 0) out vec2 vUV;
void main() {
    vUV = inPosition.xy * 0.5 + 0.5;
    gl_Position = vec4(inPosition.xy, 0.0, 1.0);
}
