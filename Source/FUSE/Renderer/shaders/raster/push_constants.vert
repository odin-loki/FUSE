#version 450
// B2 gate row "push constants correctly pass per-draw data": CommandBufferRecorder pushes
// {materialId, 0, 0, 0} (16 bytes, vertex|fragment) before every indexed draw. The draw's
// material id selects where the fixture triangle lands: 1 -> left half, 2 -> right half.
layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform DrawPush {
    uint materialId;
    uint reserved0;
    uint reserved1;
    uint reserved2;
} pc;

void main() {
    float offsetX = pc.materialId == 1u ? -0.5 : (pc.materialId == 2u ? 0.5 : 0.0);
    gl_Position = vec4(inPosition.x * 0.8 + offsetX, inPosition.y, inPosition.z, 1.0);
}
