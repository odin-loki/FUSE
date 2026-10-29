// FUSE UI (UI2): CPU reference rasteriser. See cpu_raster.hpp.
#include <fuse/ui/cpu_raster.hpp>
#include <fuse/ui/font5x7.hpp>

#include <cmath>
#include <cstdio>

namespace fuse::ui {

Vec2 measureText(std::string_view s, u32 scale) {
    if (scale == 0) scale = 1;
    if (s.empty()) return {0.f, f32(kGlyphHeight * scale)};
    return {f32((s.size() * kCellWidth - 1) * scale), f32(kGlyphHeight * scale)};
}

void Image::resize(u32 w, u32 h, Color c) {
    width = w;
    height = h;
    rgba.resize(usize(w) * h * 4u);
    const u8 r = u8(c.r * c.a / 255), g = u8(c.g * c.a / 255), b = u8(c.b * c.a / 255);
    for (usize i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = c.a;
    }
}

Color Image::at(u32 x, u32 y) const {
    const usize i = (usize(y) * width + x) * 4u;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

u64 Image::hash() const {
    u64 h = 1469598103934665603ull;
    for (u8 v : rgba) {
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

namespace {

struct IRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

IRect toPixels(const Rect& r, const Image& img) {
    IRect p{int(std::floor(r.x)), int(std::floor(r.y)), int(std::ceil(r.right())), int(std::ceil(r.bottom()))};
    p.x0 = std::max(p.x0, 0);
    p.y0 = std::max(p.y0, 0);
    p.x1 = std::min(p.x1, int(img.width));
    p.y1 = std::min(p.y1, int(img.height));
    return p;
}

void blend(Image& img, int x, int y, Color c, f32 coverage) {
    if (coverage <= 0.f) return;
    const f32 a = (f32(c.a) / 255.f) * std::min(coverage, 1.f);
    u8* px = &img.rgba[(usize(y) * img.width + usize(x)) * 4u];
    const f32 inv = 1.f - a;
    px[0] = u8(std::lround(f32(c.r) * a + f32(px[0]) * inv));
    px[1] = u8(std::lround(f32(c.g) * a + f32(px[1]) * inv));
    px[2] = u8(std::lround(f32(c.b) * a + f32(px[2]) * inv));
    px[3] = u8(std::lround(255.f * a + f32(px[3]) * inv));
}

/// Signed distance from pixel centre p to a rounded box (negative inside).
f32 roundedBoxSdf(f32 px, f32 py, const Rect& r, f32 radius) {
    const f32 hw = r.w * 0.5f, hh = r.h * 0.5f;
    radius = std::min(radius, std::min(hw, hh));
    const f32 qx = std::fabs(px - (r.x + hw)) - (hw - radius);
    const f32 qy = std::fabs(py - (r.y + hh)) - (hh - radius);
    const f32 ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
}

f32 coverageOf(f32 sdf) { return std::clamp(0.5f - sdf, 0.f, 1.f); }

void fill(Image& img, const IRect& clip, const DrawCmd& c) {
    const IRect p = toPixels(c.rect, img);
    for (int y = std::max(p.y0, clip.y0); y < std::min(p.y1, clip.y1); ++y) {
        for (int x = std::max(p.x0, clip.x0); x < std::min(p.x1, clip.x1); ++x) {
            const f32 d = roundedBoxSdf(f32(x) + 0.5f, f32(y) + 0.5f, c.rect, c.radius);
            blend(img, x, y, c.color, coverageOf(d));
        }
    }
}

void stroke(Image& img, const IRect& clip, const DrawCmd& c) {
    const IRect p = toPixels(c.rect, img);
    const Rect inner = c.rect.inset(Insets::all(c.width));
    const f32 innerRadius = std::max(0.f, c.radius - c.width);
    for (int y = std::max(p.y0, clip.y0); y < std::min(p.y1, clip.y1); ++y) {
        for (int x = std::max(p.x0, clip.x0); x < std::min(p.x1, clip.x1); ++x) {
            const f32 fx = f32(x) + 0.5f, fy = f32(y) + 0.5f;
            const f32 outer = coverageOf(roundedBoxSdf(fx, fy, c.rect, c.radius));
            const f32 in = inner.empty() ? 0.f : coverageOf(roundedBoxSdf(fx, fy, inner, innerRadius));
            blend(img, x, y, c.color, outer - in);
        }
    }
}

void text(Image& img, const IRect& clip, const DrawCmd& c, std::string_view s) {
    const int scale = int(c.scale);
    int penX = int(std::lround(c.rect.x));
    const int top = int(std::lround(c.rect.y));
    for (char ch : s) {
        const u8* rows = glyph5x7(ch);
        for (int gy = 0; gy < kGlyphHeight; ++gy) {
            for (int gx = 0; gx < kGlyphWidth; ++gx) {
                if ((rows[gy] & (0x10u >> gx)) == 0) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    const int y = top + gy * scale + sy;
                    if (y < clip.y0 || y >= clip.y1) continue;
                    for (int sx = 0; sx < scale; ++sx) {
                        const int x = penX + gx * scale + sx;
                        if (x >= clip.x0 && x < clip.x1) blend(img, x, y, c.color, 1.f);
                    }
                }
            }
        }
        penX += kCellWidth * scale;
    }
}

} // namespace

void rasterize(const DrawList& list, Image& target, const Rect& damage) {
    IRect clipStack[32];
    int depth = 0;
    clipStack[0] = damage.empty() ? IRect{0, 0, int(target.width), int(target.height)} : toPixels(damage, target);
    for (const DrawCmd& c : list.commands()) {
        const IRect& clip = clipStack[depth];
        switch (c.op) {
        case DrawOp::FillRect: fill(target, clip, c); break;
        case DrawOp::StrokeRect: stroke(target, clip, c); break;
        case DrawOp::Text: text(target, clip, c, list.textOf(c)); break;
        case DrawOp::PushClip: {
            const IRect r = toPixels(c.rect, target);
            IRect next{std::max(r.x0, clip.x0), std::max(r.y0, clip.y0), std::min(r.x1, clip.x1), std::min(r.y1, clip.y1)};
            if (depth + 1 < 32) clipStack[++depth] = next;
            break;
        }
        case DrawOp::PopClip:
            if (depth > 0) --depth;
            break;
        }
    }
}

bool writePpm(const Image& image, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
    for (usize i = 0; i < image.rgba.size(); i += 4) {
        std::fwrite(&image.rgba[i], 1, 3, f);
    }
    return std::fclose(f) == 0;
}

} // namespace fuse::ui
