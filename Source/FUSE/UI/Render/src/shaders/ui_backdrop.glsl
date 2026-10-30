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
layout(set = 0, binding = 3, std430) readonly buffer UiImages { uint data[]; } ui_images; // clip-path lists + edges
float edgeWord(uint i) { return uintBitsToFloat(ui_images.data[i]); }
#include "ui_clip.glsl"

layout(push_constant) uniform BlurPush {
    ivec4 area; // x0, y0, x1, y1 (first: ivec4 is 16-aligned)
    ivec2 srcOrigin;
    ivec2 srcSize;
    uint mode;
    uint cmd;
    uint radius;
    float inv;
    uint base; // box passes: buf at v[base], tmp at v[base + n]
    uint save; // layer passes: saved pixels at v[save]
    uint pad0;
    uint pad1;
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
// backdrop-filter colour matrix (filterPixel in Core/src/draw_list.cpp): premultiplied 0..255 in and out
vec4 applyFilter(uint at, vec4 b) {
    precise float a = b.w / 255.0;
    precise vec4 st = vec4(0.0, 0.0, 0.0, a);
    if (a > 0.0)
        for (int k = 0; k < 3; ++k) { precise float q = b[k] / 255.0 / a; st[k] = clamp(q, 0.0, 1.0); }
    precise vec4 res;
    for (int k = 0; k < 4; ++k) {
        const uint w = at + uint(k) * 5u;
        precise float v = edgeWord(w) * st.x + edgeWord(w + 1u) * st.y + edgeWord(w + 2u) * st.z + edgeWord(w + 3u) * st.w + edgeWord(w + 4u);
#ifdef UI_HDR
        res[k] = k == 3 ? clamp(v, 0.0, 1.0) : max(v, 0.0);
#else
        res[k] = clamp(v, 0.0, 1.0);
#endif
    }
    precise vec4 o;
    for (int k = 0; k < 3; ++k) o[k] = res[k] * res.w * 255.0;
    o.w = res.w * 255.0;
    return o;
}
// drop shadow under premultiplied 0..255 `f` at box (x, y): plane (v[2n..], the filtered box blurred by the shadow
// radius) alpha at (x - dx, y - dy), tinted; withShadow in Core/src/draw_list.cpp
vec4 withShadow(uint at, vec4 f, int x, int y, int w, int h, uint n) {
    const int qx = x - int(edgeWord(at + 20u)), qy = y - int(edgeWord(at + 21u));
    float sa = 0.0;
    if (qx >= 0 && qy >= 0 && qx < w && qy < h) sa = ui_scratch.v[2u * n + uint(qy * w + qx) * 4u + 3u];
    precise float sA = edgeWord(at + 25u) * (sa / 255.0);
    precise vec4 S = vec4(edgeWord(at + 22u) * sA * 255.0, edgeWord(at + 23u) * sA * 255.0, edgeWord(at + 24u) * sA * 255.0, sA * 255.0);
    precise float inv = 1.0 - f.w / 255.0;
    precise vec4 o;
    for (int k = 0; k < 4; ++k) { precise float t = S[k] * inv; o[k] = f[k] + t; }
    return o;
}
float clipCoverage(GpuCmd c, float px, float py) {
    precise float cov = 1.0;
    if (c.params.z > 0.0) cov = cov * clamp(0.5 - sdRoundRect(px, py, c.clip, vec4(c.params.z)), 0.0, 1.0);
    const uint clipList = floatBitsToUint(c.params.w);
    if (clipList != 0u) {
        const uint cnt = ui_images.data[clipList];
        for (uint q = 0u; q < cnt && cov > 0.0; ++q) {
            const uint e = clipList + 1u + q * 3u;
            cov = cov * clipPathCoverage(ui_images.data[e], ui_images.data[e + 1u], ui_images.data[e + 2u] != 0u, px, py);
        }
    }
    return cov;
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
        const uint row = pc.base + uint(y * w) * 4u + uint(k);
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
        const uint col = pc.base + id; // x * 4 + k
        const uint stride = uint(w) * 4u;
        precise float acc = 0.0;
        for (int i = -r; i <= r; ++i) acc = acc + ui_scratch.v[n + uint(clamp(i, 0, h - 1)) * stride + col];
        for (int i = 0; i < h; ++i) {
            ui_scratch.v[uint(i) * stride + col] = acc * pc.inv;
            precise float d = ui_scratch.v[n + uint(clamp(i + r + 1, 0, h - 1)) * stride + col] - ui_scratch.v[n + uint(clamp(i - r, 0, h - 1)) * stride + col];
            acc = acc + d;
        }
    } else if (pc.mode == 4u) {
        // layer begin: save the box, clear it to transparent
        if (id >= uint(w * h)) return;
        const ivec2 ip = pc.srcOrigin + ivec2(int(id) % w, int(id) / w);
        const vec4 t = imageLoad(ui_target, ip);
#ifdef UI_HDR
        for (int k = 0; k < 4; ++k) ui_scratch.v[pc.save + id * 4u + uint(k)] = t[k] * 255.0;
#else
        const uvec4 b = uvec4(floor(t * 255.0 + 0.5));
        for (int k = 0; k < 4; ++k) ui_scratch.v[pc.save + id * 4u + uint(k)] = float(b[k]);
#endif
        imageStore(ui_target, ip, vec4(0.0));
    } else if (pc.mode == 5u) {
        // colour matrix over the box; with a drop shadow, a copy to the shadow plane v[2n..]
        if (id >= uint(w * h)) return;
        const GpuCmd c = ui_cmds.cmds[pc.cmd];
        const uint i = id * 4u;
        vec4 b = vec4(ui_scratch.v[i], ui_scratch.v[i + 1u], ui_scratch.v[i + 2u], ui_scratch.v[i + 3u]);
        b = applyFilter(floatBitsToUint(c.grad2.z), b);
        for (int k = 0; k < 4; ++k) ui_scratch.v[i + uint(k)] = b[k];
        if (floatBitsToUint(c.grad2.w) == 6u)
            for (int k = 0; k < 4; ++k) ui_scratch.v[2u * n + i + uint(k)] = b[k];
    } else if (pc.mode == 6u) {
        // layer end: filtered layer (+ shadow) over the saved pixels, mixed by the clip coverage
        if (id >= uint(w * h)) return;
        const int lx = int(id) % w, ly = int(id) / w;
        const ivec2 ip = pc.srcOrigin + ivec2(lx, ly);
        const GpuCmd c = ui_cmds.cmds[pc.cmd];
        const float cov = clipCoverage(c, float(ip.x) + 0.5, float(ip.y) + 0.5);
        const uint si = pc.save + id * 4u;
        const vec4 u = vec4(ui_scratch.v[si], ui_scratch.v[si + 1u], ui_scratch.v[si + 2u], ui_scratch.v[si + 3u]);
        vec4 o = vec4(ui_scratch.v[id * 4u], ui_scratch.v[id * 4u + 1u], ui_scratch.v[id * 4u + 2u], ui_scratch.v[id * 4u + 3u]);
        if (floatBitsToUint(c.grad2.w) == 6u) o = withShadow(floatBitsToUint(c.grad2.z), o, lx, ly, w, h, n);
        precise float inv = 1.0 - o.w / 255.0;
#ifdef UI_HDR
        vec4 res = u / 255.0;
        if (cov > 0.0) {
            for (int k = 0; k < 4; ++k) {
                precise float rv = o[k] + u[k] * inv;
                precise float m = u[k] + (rv - u[k]) * cov;
                res[k] = max(m / 255.0, 0.0);
            }
            res.a = min(res.a, 1.0);
        }
        imageStore(ui_target, ip, res);
#else
        uvec4 res = uvec4(u);
        if (cov > 0.0)
            for (int k = 0; k < 4; ++k) {
                precise float rv = o[k] + u[k] * inv;
                precise float m = u[k] + (rv - u[k]) * cov;
                res[k] = uint(clamp(m >= 0.0 ? floor(m + 0.5) : ceil(m - 0.5), 0.0, 255.0)); // std::lround
            }
        imageStore(ui_target, ip, vec4(res) / 255.0);
#endif
    } else {
        const int aw = pc.area.z - pc.area.x, ah = pc.area.w - pc.area.y;
        if (id >= uint(aw * ah)) return;
        const int x = pc.area.x + int(id) % aw, y = pc.area.y + int(id) / aw;
        const GpuCmd c = ui_cmds.cmds[pc.cmd];
        const float px = float(x) + 0.5, py = float(y) + 0.5;
        precise float cov = clamp(0.5 - sdRoundRect(px, py, c.rect, c.radius), 0.0, 1.0);
        if (c.params.z > 0.0) cov = cov * clamp(0.5 - sdRoundRect(px, py, c.clip, vec4(c.params.z)), 0.0, 1.0);
        const uint clipList = floatBitsToUint(c.params.w); // active clip paths, as ui.draw_list
        if (clipList != 0u) {
            const uint cnt = ui_images.data[clipList];
            for (uint q = 0u; q < cnt && cov > 0.0; ++q) {
                const uint e = clipList + 1u + q * 3u;
                cov = cov * clipPathCoverage(ui_images.data[e], ui_images.data[e + 1u], ui_images.data[e + 2u] != 0u, px, py);
            }
        }
        if (cov <= 0.0) return;
        const ivec2 ip = ivec2(x, y);
#ifdef UI_HDR
        const vec4 d0 = imageLoad(ui_target, ip);
        const uint bi = (uint(y - pc.srcOrigin.y) * uint(w) + uint(x - pc.srcOrigin.x)) * 4u;
        vec4 bv = vec4(ui_scratch.v[bi], ui_scratch.v[bi + 1u], ui_scratch.v[bi + 2u], ui_scratch.v[bi + 3u]);
        if (floatBitsToUint(c.grad2.w) == 6u) bv = withShadow(floatBitsToUint(c.grad2.z), bv, x - pc.srcOrigin.x, y - pc.srcOrigin.y, w, h, n);
        vec4 o;
        for (int k = 0; k < 4; ++k) {
            precise float m = d0[k] + (bv[k] / 255.0 - d0[k]) * cov;
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
        vec4 bv = vec4(ui_scratch.v[bi], ui_scratch.v[bi + 1u], ui_scratch.v[bi + 2u], ui_scratch.v[bi + 3u]);
        if (floatBitsToUint(c.grad2.w) == 6u) bv = withShadow(floatBitsToUint(c.grad2.z), bv, x - pc.srcOrigin.x, y - pc.srcOrigin.y, w, h, n);
        uvec4 o;
        for (int k = 0; k < 4; ++k) {
            const float dv = float(d0[k]);
            precise float m = dv + (bv[k] - dv) * cov;
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
