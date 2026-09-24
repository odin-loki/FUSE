// FUSE Relight RL-1.6 test shader: dxbc-spirv-like I/O (NORMAL0 input by name, TEXCOORD0 at location 1,
// COLOR0 at location 9), gl_Position in gl_PerVertex, an early return and a loop.
#version 450
layout(location = 0) in vec4 v0_position0;
layout(location = 1) in vec4 v1_normal0;
layout(location = 2) in vec4 v2_texcoord0;
layout(location = 3) in vec4 v3_color;
layout(location = 1) out vec4 o1_texcoord0;
layout(location = 9) out vec4 o2_color;
layout(set = 0, binding = 0) uniform Constants {
    mat4 wvp;
    vec4 light;
} c;
void main() {
    gl_Position = c.wvp * v0_position0;
    float d = max(dot(v1_normal0.xyz, c.light.xyz), 0.0);
    o2_color = v3_color * d;
    if (v0_position0.w < 0.0) {
        o1_texcoord0 = vec4(0.0);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        d += 0.125;
    }
    o1_texcoord0 = v2_texcoord0 * d;
}
