// FUSE UI GPU backend: "ui.backdrop" — DrawOp::Backdrop (frosted glass) between ui.draw_list segments. The
// specification is backdropBlur in Core/src/draw_list.cpp: copy the source box (area + 3r margin, clamped to the
// target) to float, three box passes per axis (clamp-to-edge running sums, same operation order as
// simd::boxVertical), then mix into the target under the rounded rect by coverage and blend the tint.
// One pipeline, several modes, each its own dispatch with a barrier in between:
//   0 load (pixel per thread), 1 horizontal (row x channel per thread), 2 vertical (column x channel), 3 composite,
//   4 layer save + clear, 5 colour matrix, 6 layer composite (a band of rows), 7 SVG program op, 8 / 9 program blur
//   H / V, 10 compact copy of a layer for tiled (banded) layer ends.
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
    uint save; // layer passes: saved pixels at v[save] (layer box = area, 4 floats per pixel)
    uint instr; // mode 7..9: program instruction
    uint flags; // bit 0: mode 0 loads the band from the compact copy at v[compact] instead of the target; bit 1:
                // mode 9 clears outside the op's subregion
    uint bandY0; // mode 6: source-local rows [bandY0, bandY1) to composite
    uint bandY1;
    uint compact; // tiled layer end: the layer's pixels, 1 word (RGBA8) / 2 words (half) per pixel, area-indexed
    uint pad0;
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
// SVG filter program (runFilterProgram in Core/src/filter_program.cpp): header words at `at` = planes, margin,
// dataStops, 0, K, region x y w h, 0; op i at at + 10 + 35 i = in1, in2, out, dataIndex, op, subregion x y w h, 0,
// p0..p24; data floats at at + 10 + 35 K. Planes of n floats at v[plane * n]; (x, y) + org = target pixel.
const float kInv255 = 0.003921568859368563;
float prm(uint ob, int k) { return edgeWord(ob + 10u + uint(k)); }
vec4 plane(uint p, uint n, uint i) { const uint b = p * n + i; return vec4(ui_scratch.v[b], ui_scratch.v[b + 1u], ui_scratch.v[b + 2u], ui_scratch.v[b + 3u]); }
uint g_data; // data word base of the running op
float dat(int i) { return edgeWord(g_data + uint(i)); }
float ppow(float x, float y) { return x <= 0.0 ? (y == 0.0 ? 1.0 : 0.0) : pow(x, y); }
int posMod(int a, int m) { return a >= 0 ? a % m : (m - (-a) % m) % m; }
bool inSub(uint ob, ivec2 g) {
    precise float px = float(g.x) + 0.5;
    precise float py = float(g.y) + 0.5;
    const float sx = edgeWord(ob + 5u), sy = edgeWord(ob + 6u);
    precise float sx1 = sx + edgeWord(ob + 7u);
    precise float sy1 = sy + edgeWord(ob + 8u);
    return px >= sx && px < sx1 && py >= sy && py < sy1;
}
float alphaAt(uint p, uint n, int w, int x, int y) { return ui_scratch.v[p * n + uint(y * w + x) * 4u + 3u] * kInv255; }
float sCurve(float t) { precise float r = t * t * (3.0 - 2.0 * t); return r; }
float lerpT(float t, float a, float b) { precise float r = a + t * (b - a); return r; }
float noise2(int c, float vx, float vy, bool stitch, int nw, int nh, int wx, int wy) {
    precise float tx = vx + 4096.0;
    int bx0 = int(tx), bx1 = bx0 + 1;
    precise float rx0 = tx - float(int(tx));
    precise float rx1 = rx0 - 1.0;
    precise float ty = vy + 4096.0;
    int by0 = int(ty), by1 = by0 + 1;
    precise float ry0 = ty - float(int(ty));
    precise float ry1 = ry0 - 1.0;
    if (stitch) {
        if (bx0 >= wx) bx0 -= nw;
        if (bx1 >= wx) bx1 -= nw;
        if (by0 >= wy) by0 -= nh;
        if (by1 >= wy) by1 -= nh;
    }
    bx0 &= 255; bx1 &= 255; by0 &= 255; by1 &= 255;
    const int i = int(dat(bx0)), j = int(dat(bx1));
    const int b00 = int(dat(i + by0)), b10 = int(dat(j + by0)), b01 = int(dat(i + by1)), b11 = int(dat(j + by1));
    const float sx = sCurve(rx0), sy = sCurve(ry0);
    const int nn = 514;
    precise float u = rx0 * dat(nn + (c * nn + b00) * 2) + ry0 * dat(nn + (c * nn + b00) * 2 + 1);
    precise float v = rx1 * dat(nn + (c * nn + b10) * 2) + ry0 * dat(nn + (c * nn + b10) * 2 + 1);
    const float a = lerpT(sx, u, v);
    u = rx0 * dat(nn + (c * nn + b01) * 2) + ry1 * dat(nn + (c * nn + b01) * 2 + 1);
    v = rx1 * dat(nn + (c * nn + b11) * 2) + ry1 * dat(nn + (c * nn + b11) * 2 + 1);
    const float b = lerpT(sx, u, v);
    return lerpT(sy, a, b);
}
void runOp(uint at, uint ob, uint n, int x, int y, int w, int h, ivec2 org) {
    const uint i1 = uint(edgeWord(ob)), i2 = uint(edgeWord(ob + 1u)), io = uint(edgeWord(ob + 2u));
    const int kind = int(edgeWord(ob + 4u));
    g_data = at + 10u + 35u * uint(edgeWord(at + 4u)) + uint(edgeWord(ob + 3u));
    const uint pi = uint(y * w + x) * 4u;
    const ivec2 g = org + ivec2(x, y);
    precise vec4 r;
    const vec4 a = plane(i1, n, pi);
    const vec4 b = plane(i2, n, pi);
    if (kind == 0) { // blur: copy in1 -> out, modes 8 / 9 follow (9 clears outside the subregion)
        if (i1 == io) return;
        r = a;
    } else if (kind == 1) {
        r = applyFilter(ob + 10u, a);
    } else if (kind == 2) {
        const int qx = x - int(prm(ob, 2)), qy = y - int(prm(ob, 3));
        r = (qx >= 0 && qy >= 0 && qx < w && qy < h) ? plane(i1, n, uint(qy * w + qx) * 4u) : vec4(0.0);
    } else if (kind == 3) {
        precise float al = prm(ob, 3);
        r = vec4(prm(ob, 0) * al * 255.0, prm(ob, 1) * al * 255.0, prm(ob, 2) * al * 255.0, al * 255.0);
    } else if (kind == 4) {
        const int mode = int(prm(ob, 0));
        precise float qa = a.w * kInv255;
        precise float qb = b.w * kInv255;
        for (int k = 0; k < 3; ++k) {
            precise float ia = a[k] * (1.0 - qb);
            precise float ib = b[k] * (1.0 - qa);
            precise float ab = a[k] * b[k] * kInv255;
            precise float v;
            if (mode == 1) v = ia + ib + ab;
            else if (mode == 2) v = a[k] + b[k] - ab;
            else if (mode == 3) v = min(ia + b[k], ib + a[k]);
            else if (mode == 4) v = max(ia + b[k], ib + a[k]);
            else v = a[k] + ib;
            r[k] = v;
        }
        precise float aa = a.w * b.w * kInv255;
        r.w = a.w + b.w - aa;
    } else if (kind == 5) {
        const int opr = int(prm(ob, 0));
        precise float qa = a.w * kInv255;
        precise float qb = b.w * kInv255;
        if (opr == 5) {
            const float k1 = prm(ob, 1), k2 = prm(ob, 2), k3 = prm(ob, 3), k4 = prm(ob, 4);
            for (int k = 0; k < 4; ++k) {
                precise float v = k1 * a[k] * b[k] * kInv255 + k2 * a[k] + k3 * b[k] + k4 * 255.0;
                r[k] = clamp(v, 0.0, 255.0);
            }
            for (int k = 0; k < 3; ++k) r[k] = min(r[k], r.w);
        } else {
            for (int k = 0; k < 4; ++k) {
                precise float v;
                if (opr == 1) v = a[k] * qb;
                else if (opr == 2) v = a[k] * (1.0 - qb);
                else if (opr == 3) v = a[k] * qb + b[k] * (1.0 - qa);
                else if (opr == 4) v = a[k] * (1.0 - qb) + b[k] * (1.0 - qa);
                else v = a[k] + b[k] * (1.0 - qa);
                r[k] = v;
            }
        }
    } else if (kind == 6) {
        r = vec4(0.0, 0.0, 0.0, a.w);
    } else if (kind == 8) { // turbulence
        const float fx = prm(ob, 11), fy = prm(ob, 12);
        const int oct = clamp(int(prm(ob, 2)), 0, 10);
        const bool fractal = prm(ob, 3) != 0.0, stitch = prm(ob, 4) != 0.0;
        precise float bx = (float(g.x) - prm(ob, 5)) * fx;
        precise float by = (float(g.y) - prm(ob, 6)) * fy;
        precise vec4 res;
        for (int c = 0; c < 4; ++c) {
            int nw = int(prm(ob, 13)), nh = int(prm(ob, 14)), wx = int(prm(ob, 15)), wy = int(prm(ob, 16));
            precise float sum = 0.0;
            precise float vx = bx;
            precise float vy = by;
            precise float ratio = 1.0;
            for (int o = 0; o < oct; ++o) {
                const float nz = noise2(c, vx, vy, stitch, nw, nh, wx, wy);
                sum = sum + (fractal ? nz : abs(nz)) / ratio;
                vx = vx * 2.0; vy = vy * 2.0; ratio = ratio * 2.0;
                if (stitch) { nw *= 2; wx = 2 * wx - 4096; nh *= 2; wy = 2 * wy - 4096; }
            }
            precise float v = fractal ? (sum * 255.0 + 255.0) * 0.5 : sum * 255.0;
            res[c] = clamp(v, 0.0, 255.0);
        }
        precise float al = res.w * kInv255;
        r = vec4(res.x * al, res.y * al, res.z * al, res.w);
    } else if (kind == 9) { // lighting
        const bool l = x > 0, rr = x < w - 1, u = y > 0, dn = y < h - 1;
        const int xl = l ? x - 1 : x, xr = rr ? x + 1 : x, yu = u ? y - 1 : y, yd = dn ? y + 1 : y;
        const float wu = u ? 1.0 : 0.0, wd = dn ? 1.0 : 0.0, wl = l ? 1.0 : 0.0, wr = rr ? 1.0 : 0.0;
        const float fxx = (l && rr) ? 0.5 : 1.0, fyy = (u && dn) ? 0.5 : 1.0;
        const int sw = int(wu + wd), sh = int(wl + wr);
        const float facx = sw == 2 ? 0.5 : (sw == 1 ? 0.6666666865348816 : 1.0);
        const float facy = sh == 2 ? 0.5 : (sh == 1 ? 0.6666666865348816 : 1.0);
        precise float sxv = wu * (alphaAt(i1, n, w, xr, yu) - alphaAt(i1, n, w, xl, yu)) + 2.0 * (alphaAt(i1, n, w, xr, y) - alphaAt(i1, n, w, xl, y)) +
                            wd * (alphaAt(i1, n, w, xr, yd) - alphaAt(i1, n, w, xl, yd));
        precise float syv = wl * (alphaAt(i1, n, w, xl, yd) - alphaAt(i1, n, w, xl, yu)) + 2.0 * (alphaAt(i1, n, w, x, yd) - alphaAt(i1, n, w, x, yu)) +
                            wr * (alphaAt(i1, n, w, xr, yd) - alphaAt(i1, n, w, xr, yu));
        const float ss = prm(ob, 1);
        precise float nx = -ss * facx * fxx * sxv;
        precise float ny = -ss * facy * fyy * syv;
        precise float nl = sqrt(nx * nx + ny * ny + 1.0);
        precise float Nx = nx / nl;
        precise float Ny = ny / nl;
        precise float Nz = 1.0 / nl;
        precise float Lx = prm(ob, 8);
        precise float Ly = prm(ob, 9);
        precise float Lz = prm(ob, 10);
        const int lt = int(prm(ob, 7));
        if (lt != 0) {
            Lx = prm(ob, 8) - float(g.x); Ly = prm(ob, 9) - float(g.y); Lz = prm(ob, 10) - ss * alphaAt(i1, n, w, x, y);
            precise float ll = sqrt(Lx * Lx + Ly * Ly + Lz * Lz);
            if (ll > 0.0) { Lx = Lx / ll; Ly = Ly / ll; Lz = Lz / ll; }
        }
        precise vec3 lc = vec3(prm(ob, 4), prm(ob, 5), prm(ob, 6));
        if (lt == 2) {
            precise float d = -(Lx * prm(ob, 11) + Ly * prm(ob, 12) + Lz * prm(ob, 13));
            const float f = d < prm(ob, 15) ? 0.0 : ppow(d, prm(ob, 14));
            lc = vec3(lc.x * f, lc.y * f, lc.z * f);
        }
        if (prm(ob, 0) == 0.0) {
            precise float f = prm(ob, 2) * max(Nx * Lx + Ny * Ly + Nz * Lz, 0.0);
            for (int k = 0; k < 3; ++k) { precise float q = clamp(f * lc[k], 0.0, 1.0) * 255.0; r[k] = q; }
            r.w = 255.0;
        } else {
            precise float Hx = Lx;
            precise float Hy = Ly;
            precise float Hz = Lz + 1.0;
            precise float hl = sqrt(Hx * Hx + Hy * Hy + Hz * Hz);
            if (hl > 0.0) { Hx = Hx / hl; Hy = Hy / hl; Hz = Hz / hl; }
            precise float f = prm(ob, 2) * ppow(max(Nx * Hx + Ny * Hy + Nz * Hz, 0.0), prm(ob, 3));
            precise vec3 c;
            for (int k = 0; k < 3; ++k) c[k] = clamp(f * lc[k], 0.0, 1.0);
            const float al = max(c.x, max(c.y, c.z));
            r = vec4(c.x * 255.0, c.y * 255.0, c.z * 255.0, al * 255.0);
        }
    } else if (kind == 10) { // morphology
        const int rx = int(prm(ob, 3)), ry = int(prm(ob, 4));
        const bool dil = prm(ob, 0) != 0.0;
        if (rx == 0 && ry == 0) r = a;
        else {
            r = vec4(dil ? 0.0 : 255.0);
            for (int j = -ry; j <= ry; ++j)
                for (int i = -rx; i <= rx; ++i) {
                    const int qx = x + i, qy = y + j;
                    const vec4 v = (qx >= 0 && qy >= 0 && qx < w && qy < h) ? plane(i1, n, uint(qy * w + qx) * 4u) : vec4(0.0);
                    r = dil ? max(r, v) : min(r, v);
                }
        }
    } else if (kind == 11) { // image
        precise float fx = (float(g.x) + 0.5 - prm(ob, 0)) * prm(ob, 6);
        precise float fy = (float(g.y) + 0.5 - prm(ob, 1)) * prm(ob, 7);
        const int iw = int(prm(ob, 4)), ih = int(prm(ob, 5));
        if (iw <= 0 || ih <= 0 || fx < 0.0 || fy < 0.0 || fx >= float(iw) || fy >= float(ih)) r = vec4(0.0);
        else {
            const int u = clamp(int(floor(fx)), 0, iw - 1), v = clamp(int(floor(fy)), 0, ih - 1);
            for (int k = 0; k < 4; ++k) r[k] = dat((v * iw + u) * 4 + k);
        }
    } else if (kind == 12) { // tile
        const int tx = int(prm(ob, 4)), ty = int(prm(ob, 5)), tw = int(prm(ob, 6)), th = int(prm(ob, 7));
        const int qx = tx + posMod(g.x - tx, tw) - org.x, qy = ty + posMod(g.y - ty, th) - org.y;
        r = (qx >= 0 && qy >= 0 && qx < w && qy < h) ? plane(i1, n, uint(qy * w + qx) * 4u) : vec4(0.0);
    } else if (kind == 13) { // component transfer (one channel)
        const int ch = clamp(int(prm(ob, 0)), 0, 3), type = int(prm(ob, 1)), cnt = int(prm(ob, 5));
        const float al = a.w;
        precise float C = ch == 3 ? al * kInv255 : (al > 0.0 ? clamp(a[ch] / al, 0.0, 1.0) : 0.0);
        precise float f = C;
        if (type == 1 && cnt > 0) {
            precise float t = C * float(cnt - 1);
            const int k = clamp(int(floor(t)), 0, cnt - 1);
            f = k >= cnt - 1 ? dat(cnt - 1) : dat(k) + (t - float(k)) * (dat(k + 1) - dat(k));
        } else if (type == 2 && cnt > 0) {
            f = dat(min(int(floor(C * float(cnt))), cnt - 1));
        } else if (type == 3) {
            f = prm(ob, 2) * C + prm(ob, 3);
        } else if (type == 4) {
            f = prm(ob, 2) * ppow(C, prm(ob, 3)) + prm(ob, 4);
        }
        f = clamp(f, 0.0, 1.0);
        r = a;
        if (ch == 3) {
            precise float na = f * 255.0;
            for (int k = 0; k < 3; ++k) r[k] = al > 0.0 ? clamp(a[k] / al, 0.0, 1.0) * na : 0.0;
            r.w = na;
        } else r[ch] = f * al;
    } else if (kind == 14) { // convolve
        const int ox = int(prm(ob, 0)), oy = int(prm(ob, 1)), tx = int(prm(ob, 4)), ty = int(prm(ob, 5)), edge = int(prm(ob, 6));
        const bool pa = prm(ob, 7) != 0.0;
        precise vec4 sum = vec4(0.0);
        for (int j = 0; j < oy; ++j)
            for (int i = 0; i < ox; ++i) {
                int qx = x - tx + i, qy = y - ty + j;
                bool inb = true;
                if (edge == 0) { qx = clamp(qx, 0, w - 1); qy = clamp(qy, 0, h - 1); }
                else if (edge == 1) { qx = posMod(qx, w); qy = posMod(qy, h); }
                else inb = qx >= 0 && qy >= 0 && qx < w && qy < h;
                if (!inb) continue;
                const vec4 s = plane(i1, n, uint(qy * w + qx) * 4u);
                const float kv = dat(ox * oy - 1 - (j * ox + i));
                for (int k = 0; k < 4; ++k) {
                    precise float v = s[k];
                    if (pa && k < 3) v = s.w > 0.0 ? s[k] / s.w * 255.0 : 0.0;
                    sum[k] = sum[k] + kv * v;
                }
            }
        for (int k = 0; k < 4; ++k) { precise float q = sum[k] * prm(ob, 2) + prm(ob, 3) * 255.0; r[k] = clamp(q, 0.0, 255.0); }
        if (pa) {
            precise float qa = a.w * kInv255;
            for (int k = 0; k < 3; ++k) r[k] = r[k] * qa;
            r.w = a.w;
        } else
            for (int k = 0; k < 3; ++k) r[k] = min(r[k], r.w);
    } else if (kind == 15) { // displacement map
        const int cx = clamp(int(prm(ob, 1)), 0, 3), cy = clamp(int(prm(ob, 2)), 0, 3);
        const float chx = cx == 3 ? b.w * kInv255 : (b.w > 0.0 ? clamp(b[cx] / b.w, 0.0, 1.0) : 0.0);
        const float chy = cy == 3 ? b.w * kInv255 : (b.w > 0.0 ? clamp(b[cy] / b.w, 0.0, 1.0) : 0.0);
        precise float dx = prm(ob, 0) * (chx - 0.5);
        precise float dy = prm(ob, 0) * (chy - 0.5);
        const int qx = int(floor(float(x) + dx + 0.5)), qy = int(floor(float(y) + dy + 0.5));
        r = (qx >= 0 && qy >= 0 && qx < w && qy < h) ? plane(i1, n, uint(qy * w + qx) * 4u) : vec4(0.0);
    } else {
        r = a;
    }
    if (kind != 0 && !inSub(ob, g)) r = vec4(0.0);
    for (int k = 0; k < 4; ++k) ui_scratch.v[io * n + pi + uint(k)] = r[k];
}
// a transform layer's pixel at box-local (x, y) as mode 0 loaded it (0 outside the box): the whole box is in the
// buffer untiled, in the compact copy when tiled
vec4 layerTexel(int x, int y, int aw, int ah) {
    if (x < 0 || y < 0 || x >= aw || y >= ah) return vec4(0.0);
    const uint ci = uint(y * aw + x);
    if ((pc.flags & 1u) == 0u) return vec4(ui_scratch.v[ci * 4u], ui_scratch.v[ci * 4u + 1u], ui_scratch.v[ci * 4u + 2u], ui_scratch.v[ci * 4u + 3u]);
#ifdef UI_HDR
    const uint c0 = floatBitsToUint(ui_scratch.v[pc.compact + ci * 2u]), c1 = floatBitsToUint(ui_scratch.v[pc.compact + ci * 2u + 1u]);
    return vec4(unpackHalf2x16(c0), unpackHalf2x16(c1)) * 255.0;
#else
    return floor(unpackUnorm4x8(floatBitsToUint(ui_scratch.v[pc.compact + ci])) * 255.0 + 0.5);
#endif
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
        vec4 t;
        if ((pc.flags & 1u) != 0u) { // tiled layer end: from the compact copy (exactly what imageLoad gave)
            const ivec2 g = pc.srcOrigin + ivec2(x, y) - pc.area.xy;
            const uint ci = uint(g.y * (pc.area.z - pc.area.x) + g.x);
#ifdef UI_HDR
            const uint c0 = floatBitsToUint(ui_scratch.v[pc.compact + ci * 2u]), c1 = floatBitsToUint(ui_scratch.v[pc.compact + ci * 2u + 1u]);
            t = vec4(unpackHalf2x16(c0), unpackHalf2x16(c1));
#else
            t = unpackUnorm4x8(floatBitsToUint(ui_scratch.v[pc.compact + ci]));
#endif
        } else {
            t = imageLoad(ui_target, pc.srcOrigin + ivec2(x, y));
        }
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
        if (id >= uint(w) * (pc.bandY1 - pc.bandY0)) return;
        const int lx = int(id) % w, ly = int(pc.bandY0) + int(id) / w;
        const uint li = uint(ly * w + lx);
        const ivec2 ip = pc.srcOrigin + ivec2(lx, ly);
        const GpuCmd c = ui_cmds.cmds[pc.cmd];
        // a chained (inner) layer of a CSS-order chain: the outermost applies the clip coverage
        const float cov = c.grad2.x != 0.0 ? 1.0 : clipCoverage(c, float(ip.x) + 0.5, float(ip.y) + 0.5);
        const ivec2 g = ip - pc.area.xy;
        const uint si = pc.save + uint(g.y * (pc.area.z - pc.area.x) + g.x) * 4u;
        const vec4 u = vec4(ui_scratch.v[si], ui_scratch.v[si + 1u], ui_scratch.v[si + 2u], ui_scratch.v[si + 3u]);
        vec4 o;
        if (c.grad2.y == 2.0) { // transform layer: bilinear resample through the pixel -> local map (sampleLayerBilinear)
            const int aw = pc.area.z - pc.area.x, ah = pc.area.w - pc.area.y;
            precise float px = float(ip.x) + 0.5;
            precise float py = float(ip.y) + 0.5;
            precise float qx = c.grad.x * px + c.grad.z * py + c.grad2.z;
            precise float qy = c.grad.y * px + c.grad.w * py + c.grad2.w;
            precise float fx = qx - float(pc.area.x);
            precise float fy = qy - float(pc.area.y);
            precise float sx = clamp(fx - 0.5, -2.0, float(aw) + 1.0);
            precise float sy = clamp(fy - 0.5, -2.0, float(ah) + 1.0);
            const float flx = floor(sx), fly = floor(sy);
            const int x0 = int(flx), y0 = int(fly);
            precise float tx = sx - flx;
            precise float ty = sy - fly;
            const vec4 p00 = layerTexel(x0, y0, aw, ah), p10 = layerTexel(x0 + 1, y0, aw, ah);
            const vec4 p01 = layerTexel(x0, y0 + 1, aw, ah), p11 = layerTexel(x0 + 1, y0 + 1, aw, ah);
            for (int k = 0; k < 4; ++k) {
                precise float top = p00[k] + (p10[k] - p00[k]) * tx;
                precise float bot = p01[k] + (p11[k] - p01[k]) * tx;
                o[k] = top + (bot - top) * ty;
            }
        } else {
            o = vec4(ui_scratch.v[li * 4u], ui_scratch.v[li * 4u + 1u], ui_scratch.v[li * 4u + 2u], ui_scratch.v[li * 4u + 3u]);
            if (floatBitsToUint(c.grad2.w) == 6u) o = withShadow(floatBitsToUint(c.grad2.z), o, lx, ly, w, h, n);
        }
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
    } else if (pc.mode == 7u) {
        if (id >= uint(w * h)) return;
        const GpuCmd c = ui_cmds.cmds[pc.cmd];
        const uint at = floatBitsToUint(c.grad2.z);
        runOp(at, at + 10u + 35u * pc.instr, n, int(id) % w, int(id) / w, w, h, pc.srcOrigin);
    } else if (pc.mode == 8u) {
        // program blur, out -> plane 1 along x (as mode 1)
        if (id >= uint(h * 4)) return;
        const GpuCmd c = ui_cmds.cmds[pc.cmd];
        const uint ob = floatBitsToUint(c.grad2.z) + 10u + 35u * pc.instr;
        const uint io = uint(edgeWord(ob + 2u));
        const int rr = int(prm(ob, 2));
        const float iv = prm(ob, 4);
        const int y = int(id) / 4, k = int(id) % 4;
        const uint row = uint(y * w) * 4u + uint(k);
        const uint src = io * n + row, dst = n + row;
        precise float acc = 0.0;
        for (int i = -rr; i <= rr; ++i) acc = acc + ui_scratch.v[src + uint(clamp(i, 0, w - 1)) * 4u];
        for (int i = 0; i < w; ++i) {
            ui_scratch.v[dst + uint(i) * 4u] = acc * iv;
            precise float d = ui_scratch.v[src + uint(clamp(i + rr + 1, 0, w - 1)) * 4u] - ui_scratch.v[src + uint(clamp(i - rr, 0, w - 1)) * 4u];
            acc = acc + d;
        }
    } else if (pc.mode == 9u) {
        // program blur, plane 1 -> out along y (as mode 2)
        if (id >= uint(w * 4)) return;
        const GpuCmd c = ui_cmds.cmds[pc.cmd];
        const uint ob = floatBitsToUint(c.grad2.z) + 10u + 35u * pc.instr;
        const uint io = uint(edgeWord(ob + 2u));
        const int rr = int(prm(ob, 3));
        const float iv = prm(ob, 5);
        const uint stride = uint(w) * 4u;
        const uint src = n + id, dst = io * n + id;
        precise float acc = 0.0;
        for (int i = -rr; i <= rr; ++i) acc = acc + ui_scratch.v[src + uint(clamp(i, 0, h - 1)) * stride];
        const bool clipSub = (pc.flags & 2u) != 0u;
        for (int i = 0; i < h; ++i) {
            ui_scratch.v[dst + uint(i) * stride] = (clipSub && !inSub(ob, pc.srcOrigin + ivec2(int(id) / 4, i))) ? 0.0 : acc * iv;
            precise float d = ui_scratch.v[src + uint(clamp(i + rr + 1, 0, h - 1)) * stride] - ui_scratch.v[src + uint(clamp(i - rr, 0, h - 1)) * stride];
            acc = acc + d;
        }
    } else if (pc.mode == 10u) {
        // tiled layer end: compact copy of the layer box (area) before any band writes the target
        const int aw = pc.area.z - pc.area.x, ah = pc.area.w - pc.area.y;
        if (id >= uint(aw * ah)) return;
        const vec4 t = imageLoad(ui_target, pc.area.xy + ivec2(int(id) % aw, int(id) / aw));
#ifdef UI_HDR
        ui_scratch.v[pc.compact + id * 2u] = uintBitsToFloat(packHalf2x16(t.xy));
        ui_scratch.v[pc.compact + id * 2u + 1u] = uintBitsToFloat(packHalf2x16(t.zw));
#else
        ui_scratch.v[pc.compact + id] = uintBitsToFloat(packUnorm4x8(t));
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
