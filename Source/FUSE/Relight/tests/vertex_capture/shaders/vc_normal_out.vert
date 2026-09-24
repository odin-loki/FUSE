// FUSE Relight RL-1.6 test shader: a NORMAL0 output (vec3 at location 0), a partial TEXCOORD0
// (vec2 at location 1, component 0), no COLOR0, and gl_VertexIndex already in use.
#version 450
layout(location = 0) in vec3 pos;
layout(location = 0) out vec3 nrm;
layout(location = 1, component = 0) out vec2 uv;
void main() {
    gl_Position = vec4(pos, 1.0);
    nrm = vec3(float(gl_VertexIndex), 0.0, 1.0);
    uv = pos.xy;
}
