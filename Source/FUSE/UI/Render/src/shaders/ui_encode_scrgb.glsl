// FUSE UI GPU backend: "ui.encode_scrgb" — converts the RGBA8 sRGB canvas into the rgba16f output image over a box.
// scRGB (VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT): linear sRGB primaries, 1.0 = 80 nits, SDR white at pc.sdrWhite nits
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
#ifdef UI_HDR
layout(set = 0, binding = 0, rgba16f) uniform readonly image2D ui_src; // HDR canvas: extended sRGB, may exceed 1
#else
layout(set = 0, binding = 0, rgba8) uniform readonly image2D ui_src;
#endif
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D ui_dst;
layout(push_constant) uniform EncodePush {
    ivec4 area; // x0, y0, x1, y1
    float sdrWhite;
} pc;

float srgbToLinear(float v) { v = max(v, 0.0); return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
float pq(float nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float p = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy) + pc.area.xy;
    if (p.x >= pc.area.z || p.y >= pc.area.w) return;
    const vec4 s = imageLoad(ui_src, p);
    const vec3 l = vec3(srgbToLinear(s.r), srgbToLinear(s.g), srgbToLinear(s.b));
    const vec3 o = l * (pc.sdrWhite / 80.0);
    imageStore(ui_dst, p, vec4(o, s.a));
}
