#version 450
#extension GL_GOOGLE_include_directive : require
// FUSE UI SDF-quad backend, fragment stage: the coverage of one draw-list command at this pixel, computed exactly as the
// compute kernel (ui_draw_list.comp) does, output premultiplied; fixed-function blending (ONE, ONE_MINUS_SRC_ALPHA into
// RGBA8) does the over. The arithmetic before the blend matches the CPU reference; the blend itself is the hardware's
// (fuse_ui_vk_sdf_quads documents the tolerance).

struct GpuCmd {
    vec4 rect;
    vec4 fill;
    vec4 fill2;
    vec4 border;
    vec4 radius;
    vec4 clip;
    vec4 params;
    uvec4 info;
};
layout(set = 0, binding = 1, std430) readonly buffer UiCmds { GpuCmd cmds[]; } ui_cmds;
layout(set = 0, binding = 3, std430) readonly buffer UiAtlas { uint words[]; } ui_atlas;
layout(set = 0, binding = 4, std430) readonly buffer UiImages { uint data[]; } ui_images;
layout(set = 0, binding = 5) uniform sampler2D ui_ext0;
layout(set = 0, binding = 6) uniform sampler2D ui_ext1;
layout(set = 0, binding = 7) uniform sampler2D ui_ext2;
layout(set = 0, binding = 8) uniform sampler2D ui_ext3;

layout(push_constant) uniform UiPush {
    uvec2 extent;
    uint tilesX;
    uint atlasSize;
    uint tileCount;
    uint imageCount;
    uint tileBase;
    uint tileOrigin;
    uint mode;
    uint cmd;
    uint src;
    uint dst;
} pc;

layout(location = 0) flat in uint v_cmd;
layout(location = 0) out vec4 o_color;

#include "ui_math.glsl"

const uint kRect = 0u, kShadow = 1u, kGlyph = 2u, kImage = 3u, kLine = 4u;

float inv255(uint k) { return divRn(float(k), 255.0); }

vec4 premul(vec4 c, float cov) {
    precise float a = clamp(c.a, 0.0, 1.0) * cov;
    return vec4(c.r * a, c.g * a, c.b * a, a);
}
ivec2 extSize(uint e) {
    if (e == 0u) return textureSize(ui_ext0, 0);
    if (e == 1u) return textureSize(ui_ext1, 0);
    if (e == 2u) return textureSize(ui_ext2, 0);
    return textureSize(ui_ext3, 0);
}
vec4 extFetch(uint e, ivec2 q) {
    if (e == 0u) return texelFetch(ui_ext0, q, 0);
    if (e == 1u) return texelFetch(ui_ext1, q, 0);
    if (e == 2u) return texelFetch(ui_ext2, q, 0);
    return texelFetch(ui_ext3, q, 0);
}
uint atlasByte(uint idx) { return (ui_atlas.words[idx >> 2] >> ((idx & 3u) * 8u)) & 0xFFu; }

#define GLYPH_INV255(k) inv255(k)
#include "ui_glyph.glsl"

void main() {
    const GpuCmd c = ui_cmds.cmds[v_cmd];
    const uint op = c.info.x & 0xFFu;
    const uint gradient = (c.info.x >> 8) & 0xFFu;
    const ivec2 ip = ivec2(gl_FragCoord.xy);
    const float px = float(ip.x) + 0.5, py = float(ip.y) + 0.5;
    float clipCov = 1.0;
    if (c.params.z > 0.0) clipCov = clamp(0.5 - sdRoundRect(px, py, c.clip, vec4(c.params.z)), 0.0, 1.0);
    if (clipCov <= 0.0) discard;
    vec4 s = vec4(0.0);
    if (op == kRect) {
        const float d = sdRoundRect(px, py, c.rect, c.radius);
        precise float outer = clamp(0.5 - d, 0.0, 1.0) * clipCov;
        if (outer <= 0.0) discard;
        vec4 fill = c.fill;
        if (gradient != 0u) {
            float t = gradient == 1u ? clamp(divRn(py - c.rect.y, max(c.rect.w, 1.0)), 0.0, 1.0)
                                     : clamp(divRn(px - c.rect.x, max(c.rect.z, 1.0)), 0.0, 1.0);
            precise vec4 f = fill + (c.fill2 - fill) * t;
            fill = f;
        }
        if (c.params.x > 0.0 && c.border.a > 0.0) {
            precise float inner = clamp(0.5 - (d + c.params.x), 0.0, 1.0) * clipCov;
            const vec4 pf = premul(fill, inner);
            const vec4 pb = premul(c.border, outer - inner);
            precise vec4 sum = pf + pb;
            s = sum;
        } else {
            s = premul(fill, outer);
        }
    } else if (op == kShadow) {
        const float d = sdRoundRect(px, py, c.rect, c.radius);
        const float bl = max(c.params.y, 0.5);
        precise float t = clamp(divRn(d + bl, 2.0 * bl), 0.0, 1.0);
        precise float cov = (1.0 - t * t * (3.0 - 2.0 * t)) * clipCov;
        s = premul(c.fill, cov);
    } else if (op == kGlyph) {
        if (gradient == kGlyphMsdf) {
            if (pc.atlasSize == 0u) discard;
            const float cov = glyphMsdfCoverage(c.rect, c.info.y, c.info.z, c.params.y, px, py) * clipCov;
            if (cov <= 0.0) discard;
            s = premul(c.fill, cov);
            o_color = s;
            return;
        }
        const int gx = ip.x - int(c.rect.x), gy = ip.y - int(c.rect.y);
        const int tw = int(c.info.z & 0xFFFFu), th = int(c.info.z >> 16);
        if (gx < 0 || gy < 0 || gx >= tw || gy >= th || pc.atlasSize == 0u) discard;
        if (gradient == kGlyphColor) {
            s = glyphColorSource(c.info.y, gx, gy, c.fill.a, clipCov);
            if (s.a <= 0.0 && s.r <= 0.0 && s.g <= 0.0 && s.b <= 0.0) discard;
            o_color = s;
            return;
        }
        const uint u = c.info.y & 0xFFFFu, v = c.info.y >> 16;
        const uint a = atlasByte((v + uint(gy)) * pc.atlasSize + u + uint(gx));
        if (a == 0u) discard;
        s = premul(c.fill, inv255(a) * clipCov);
    } else if (op == kImage) {
        const uint img = c.info.w;
        if (img >= pc.imageCount) discard;
        const uint ext = ui_images.data[img * 4u + 3u];
        if (ext != 0u) {
            const ivec2 es = max(extSize(ext - 1u), ivec2(1));
            const int ex = clamp(int(divRn(px - c.rect.x, c.rect.z) * float(es.x)), 0, es.x - 1);
            const int ey = clamp(int(divRn(py - c.rect.y, c.rect.w) * float(es.y)), 0, es.y - 1);
            const vec4 t = clamp(extFetch(ext - 1u, ivec2(ex, ey)), 0.0, 1.0);
            const float ek = clipCov * (c.fill.a > 0.0 ? c.fill.a : 1.0);
            s = vec4(t.rgb * t.a * ek, t.a * ek);
        } else {
            const uint off = ui_images.data[img * 4u], w = ui_images.data[img * 4u + 1u], h = ui_images.data[img * 4u + 2u];
            const int sx = clamp(int(divRn(px - c.rect.x, c.rect.z) * float(w)), 0, int(w) - 1);
            const int sy = clamp(int(divRn(py - c.rect.y, c.rect.w) * float(h)), 0, int(h) - 1);
            const uint t = ui_images.data[off + uint(sy) * w + uint(sx)];
            const float kk = clipCov * (c.fill.a > 0.0 ? c.fill.a : 1.0);
            s = vec4(inv255(t & 0xFFu) * kk, inv255((t >> 8) & 0xFFu) * kk, inv255((t >> 16) & 0xFFu) * kk, inv255(t >> 24) * kk);
        }
    } else if (op == kLine) {
        precise float vx = c.rect.z - c.rect.x;
        precise float vy = c.rect.w - c.rect.y;
        precise float len2 = max(vx * vx + vy * vy, 1e-6);
        precise float t = clamp(divRn((px - c.rect.x) * vx + (py - c.rect.y) * vy, len2), 0.0, 1.0);
        precise float ex = px - (c.rect.x + vx * t);
        precise float ey = py - (c.rect.y + vy * t);
        precise float d = sqrtRn(ex * ex + ey * ey) - c.params.x * 0.5;
        precise float cov = clamp(0.5 - d, 0.0, 1.0) * clipCov;
        if (cov <= 0.0) discard;
        s = premul(c.fill, cov);
    } else {
        discard;
    }
    if (s.a <= 0.0 && s.r <= 0.0 && s.g <= 0.0 && s.b <= 0.0) discard;
    o_color = s;
}
