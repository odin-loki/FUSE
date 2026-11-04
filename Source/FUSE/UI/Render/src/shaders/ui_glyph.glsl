// FUSE UI colour / MSDF glyph sampling, included by ui_draw_list.comp and ui_sdf_quad.frag after their GpuCmd, UiAtlas,
// push constants, divRn and a GLYPH_INV255(k) macro (k / 255 correctly rounded). The specification is CpuRasterizer's
// colorTexel / msdfCoverage (Core/src/draw_list.cpp): same operations in the same order.
// The atlas buffer holds the size x size coverage page, then the RGBA8 colour page (side size / 2). Few helper calls
// and plain parameters: every GLSL call copies its arguments in SPIR-V, and the kernel is embedded in every binary.
#ifndef FUSE_UI_GLYPH_GLSL
#define FUSE_UI_GLYPH_GLSL

const uint kGlyphCoverage = 0u, kGlyphColor = 1u, kGlyphMsdf = 2u;

#define GLYPH_TEXEL(x, y) ui_atlas.words[(pc.atlasSize * pc.atlasSize) / 4u + (y) * (pc.atlasSize / 2u) + (x)]

vec3 glyphRgb(uint w) { return vec3(GLYPH_INV255(w & 0xFFu), GLYPH_INV255((w >> 8) & 0xFFu), GLYPH_INV255((w >> 16) & 0xFFu)); }

// MSDF texels stretched over the command rect, bilinear (edge-clamped), median, times the screen distance range.
float glyphMsdfCoverage(vec4 rect, uint uv, uint size, float range, float px, float py) {
    const int tw = int(size & 0xFFFFu), th = int(size >> 16);
    if (tw == 0 || th == 0 || rect.z <= 0.0 || rect.w <= 0.0) return 0.0;
    const uint u = uv & 0xFFFFu, v = uv >> 16;
    precise float sx = divRn((px - rect.x) * float(tw), rect.z) - 0.5;
    precise float sy = divRn((py - rect.y) * float(th), rect.w) - 0.5;
    const float fx0 = floor(sx), fy0 = floor(sy);
    precise float fx = sx - fx0;
    precise float fy = sy - fy0;
    const uint x0 = u + uint(clamp(int(fx0), 0, tw - 1)), x1 = u + uint(clamp(int(fx0) + 1, 0, tw - 1));
    const uint y0 = v + uint(clamp(int(fy0), 0, th - 1)), y1 = v + uint(clamp(int(fy0) + 1, 0, th - 1));
    const vec3 a00 = glyphRgb(GLYPH_TEXEL(x0, y0)), a10 = glyphRgb(GLYPH_TEXEL(x1, y0));
    const vec3 a01 = glyphRgb(GLYPH_TEXEL(x0, y1)), a11 = glyphRgb(GLYPH_TEXEL(x1, y1));
    precise vec3 top = a00 + (a10 - a00) * fx;
    precise vec3 bot = a01 + (a11 - a01) * fx;
    precise vec3 ch = top + (bot - top) * fy;
    const float med = max(min(ch.r, ch.g), min(max(ch.r, ch.g), ch.b));
    precise float spr = divRn(range * rect.z, float(tw));
    precise float cov = spr * (med - 0.5) + 0.5;
    return clamp(cov, 0.0, 1.0);
}

// Premultiplied source of a colour glyph texel at glyph-local (gx, gy), times fill.a and the clip coverage.
vec4 glyphColorSource(uint uv, int gx, int gy, float alpha, float clipCov) {
    const uint w = GLYPH_TEXEL((uv & 0xFFFFu) + uint(gx), (uv >> 16) + uint(gy));
    precise float k = clamp(alpha, 0.0, 1.0) * clipCov;
    precise vec4 o = vec4(glyphRgb(w), GLYPH_INV255(w >> 24)) * k;
    return o;
}

#endif
