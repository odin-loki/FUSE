#version 450
// Per-draw colour from the same push-constant block: material 1 red, 2 green, anything else blue.
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform DrawPush {
    uint materialId;
    uint reserved0;
    uint reserved1;
    uint reserved2;
} pc;

void main() {
    if (pc.materialId == 1u) {
        outColor = vec4(1.0, 0.0, 0.0, 1.0);
    } else if (pc.materialId == 2u) {
        outColor = vec4(0.0, 1.0, 0.0, 1.0);
    } else {
        outColor = vec4(0.0, 0.0, 1.0, 1.0);
    }
}
