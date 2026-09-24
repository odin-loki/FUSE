// FUSE Relight RL-4.2: full-screen triangle for the deferred lighting pass.
#version 460

void main() {
    const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
