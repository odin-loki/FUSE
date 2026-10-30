// FUSE UI GPU backend: "ui.backdrop" — DrawOp::Backdrop (frosted glass) between ui.draw_list segments. The
// specification is backdropBlur in Core/src/draw_list.cpp: copy the source box (area + 3r margin, clamped to the
// target) to float, three box passes per axis (clamp-to-edge running sums, same operation order as
// simd::boxVertical), then mix into the target under the rounded rect by coverage and blend the tint.
// One pipeline, four modes, each its own dispatch with a barrier in between:
//   0 load (pixel per thread), 1 horizontal (row x channel per thread), 2 vertical (column x channel), 3 composite.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct GpuCmd {
    vec4 rect;
    vec4 fill;
    vec4 fill2;
    vec4 border;
    vec4 radius;
    vec4 clip;
    vec4 params; // borderWidth, blur, clipRadius, 0
    uvec4 info;
    vec4 grad;  // multi-stop gradient (unused here; keeps the 160-byte std430 stride of ui_draw_list.comp)
    vec4 grad2;
};

#ifdef UI_HDR
layout(set = 0, binding = 0, rgba16f) uniform image2D ui_target; // HDR variant: float throughout, no 8-bit rounding
#else
layout(set = 0, binding = 0, rgba8) uniform image2D ui_target;
#endif
layout(set = 0, binding = 1, std430) readonly buffer UiCmds { GpuCmd cmds[]; } ui_cmds;
layout(set = 0, binding = 2, std430) buffer UiScratch { float v[]; } ui_scratch; // buf[w*h*4], then tmp[w*h*4]

layout(push_constant) uniform BlurPush {
    ivec4 area; // x0, y0, x1, y1 (first: ivec4 is 16-aligned)
    ivec2 srcOrigin;
    ivec2 srcSize;
    uint mode;
    uint cmd;
    uint radius;
    float inv;
} pc;

float sdRoundRect(float px, float py, vec4 r, vec4 rad) {
    precise float cx = r.x + r.z * 0.5;
    precise float cy = r.y + r.w * 0.5;
    precise float hx = r.z * 0.5;
    precise float hy = r.w * 0.5;
    precise float dx = px - cx;
    precise float dy = py - cy;
    float rr = dx < 0.0 ? (dy < 0.0 ? rad.x : rad.w) : (dy < 0.0 ? rad.y : rad.z);
    rr = min(rr, min(hx, hy));
    precise float qx = abs(dx) - hx + rr;
    precise float qy = abs(dy) - hy + rr;
    precise float ox = max(qx, 0.0);
    precise float oy = max(qy, 0.0);
    precise float l2 = ox * ox + oy * oy;
    return sqrt(l2) + min(max(qx, qy), 0.0) - rr;
}
uint q8(float v) { return uint(floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5)); }

void main() {
    const uint id = gl_GlobalInvocationID.x;
    const int w = pc.srcSize.x, h = pc.srcSize.y;
    const uint n = uint(w * h) * 4u; // tmp offset
    const int r = int(pc.radius);
    if (pc.mode == 0u) {
        if (id >= uint(w * h)) return;
        const int x = int(id) % w, y = int(id) / w;
        const vec4 t = imageLoad(ui_target, pc.srcOrigin + ivec2(x, y));
#ifdef UI_HDR
        for (int k = 0; k < 4; ++k) ui_scratch.v[id * 4u + uint(k)] = t[k] * 255.0;
#else
        const uvec4 b = uvec4(floor(t * 255.0 + 0.5));
        for (int k = 0; k < 4; ++k) ui_scratch.v[id * 4u + uint(k)] = float(b[k]);
#endif
    } else if (pc.mode == 1u) {
        // buf -> tmp along x: one (row, channel) per thread
        if (id >= uint(h * 4)) return;
        const int y = int(id) / 4, k = int(id) % 4;
        const uint row = uint(y * w) * 4u + uint(k);
        precise float acc = 0.0;
        for (int i = -r; i <= r; ++i) acc = acc + ui_scratch.v[row + uint(clamp(i, 0, w - 1)) * 4u];
        for (int i = 0; i < w; ++i) {
            ui_scratch.v[n + row + uint(i) * 4u] = acc * pc.inv;
            precise float d = ui_scratch.v[row + uint(clamp(i + r + 1, 0, w - 1)) * 4u] - ui_scratch.v[row + uint(clamp(i - r, 0, w - 1)) * 4u];
            acc = acc + d;
        }
    } else if (pc.mode == 2u) {
        // tmp -> buf along y: one (column, channel) per thread
        if (id >= uint(w * 4)) return;
        const uint col = id; // x * 4 + k
        const uint stride = uint(w) * 4u;
        precise float acc = 0.0;
        for (int i = -r; i <= r; ++i) acc = acc + ui_scratch.v[n + uint(clamp(i, 0, h - 1)) * stride + col];
        for (int i = 0; i < h; ++i) {
            ui_scratch.v[uint(i) * stride + col] = acc * pc.inv;
            precise float d = ui_scratch.v[n + uint(clamp(i + r + 1, 0, h - 1)) * stride + col] - ui_scratch.v[n + uint(clamp(i - r, 0, h - 1)) * stride + col];
            acc = acc + d;
        }
    } else {
        const int aw = pc.area.z - pc.area.x, ah = pc.area.w - pc.area.y;
        if (id >= uint(aw * ah)) return;
        const int x = pc.area.x + int(id) % aw, y = pc.area.y + int(id) / aw;
        const GpuCmd c = ui_cmds.cmds[pc.cmd];
        const float px = float(x) + 0.5, py = float(y) + 0.5;
        precise float cov = clamp(0.5 - sdRoundRect(px, py, c.rect, c.radius), 0.0, 1.0);
        if (c.params.z > 0.0) cov = cov * clamp(0.5 - sdRoundRect(px, py, c.clip, vec4(c.params.z)), 0.0, 1.0);
        if (cov <= 0.0) return;
        const ivec2 ip = ivec2(x, y);
#ifdef UI_HDR
        const vec4 d0 = imageLoad(ui_target, ip);
        const uint bi = (uint(y - pc.srcOrigin.y) * uint(w) + uint(x - pc.srcOrigin.x)) * 4u;
        vec4 o;
        for (int k = 0; k < 4; ++k) {
            precise float m = d0[k] + (ui_scratch.v[bi + uint(k)] / 255.0 - d0[k]) * cov;
            o[k] = max(m, 0.0);
        }
        o.a = min(o.a, 1.0);
        if (c.fill.a > 0.0) {
            precise float a = clamp(c.fill.a, 0.0, 1.0) * cov;
            const vec4 s = vec4(c.fill.r * a, c.fill.g * a, c.fill.b * a, a);
            precise vec4 res = s + o * (1.0 - s.a);
            o = vec4(max(res.rgb, vec3(0.0)), clamp(res.a, 0.0, 1.0));
        }
        imageStore(ui_target, ip, o);
#else
        const uvec4 d0 = uvec4(floor(imageLoad(ui_target, ip) * 255.0 + 0.5));
        const uint bi = (uint(y - pc.srcOrigin.y) * uint(w) + uint(x - pc.srcOrigin.x)) * 4u;
        uvec4 o;
        for (int k = 0; k < 4; ++k) {
            const float dv = float(d0[k]);
            precise float m = dv + (ui_scratch.v[bi + uint(k)] - dv) * cov;
            o[k] = uint(clamp(m >= 0.0 ? floor(m + 0.5) : ceil(m - 0.5), 0.0, 255.0)); // std::lround
        }
        if (c.fill.a > 0.0) {
            precise float a = clamp(c.fill.a, 0.0, 1.0) * cov;
            const vec4 s = vec4(c.fill.r * a, c.fill.g * a, c.fill.b * a, a);
            precise float inv = 1.0 - s.a;
            precise vec4 dd = vec4(o) / 255.0;
            precise vec4 res = s + dd * inv;
            o = uvec4(q8(res.r), q8(res.g), q8(res.b), q8(res.a));
        }
        imageStore(ui_target, ip, vec4(o) / 255.0);
#endif
    }
}
