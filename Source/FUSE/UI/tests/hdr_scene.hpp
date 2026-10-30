// Shared float-raster scene (fuse_ui_cpu_hdr, fuse_ui_vk_hdr): every CPU draw op with SDR and color::hdr colours.
#pragma once
#include "fuse/ui/color_space.hpp"
#include "fuse/ui/draw_list.hpp"

namespace fuse::ui::test {
/// 488 x 400: stripes, gradients (2-colour + stops, HDR stop), rounded border, shadow, path fill + stroke, line,
/// rounded clip, backdrop blur with tint, CSS filter layer (blur + sepia + drop shadow), HDR rects, image `img`.
/// `hdr` false: the same scene with every HDR colour at SDR brightness (for 8-bit comparisons).
inline void buildHdrScene(DrawList& dl, u32 img = ~0u, bool hdr = true) {
    namespace C = fuse::ui::color;
    const Color white4 = C::hdr(Color{1, 1, 1, 1}, hdr ? 4.f : 1.f);
    for (int i = 0; i < 12; ++i) dl.rect({f32(i * 40), 0, 20, 400}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
    dl.gradientRect({10, 10, 150, 60}, Color::rgb(0x102030), Color::rgb(0xf0e0d0), false, {8, 8, 8, 8});
    dl.gradientRect({170, 10, 150, 60},
                    Gradient::linear({170, 10}, {320, 70}, {{0.f, Color::rgb(0x0000ff)}, {0.5f, white4}, {1.f, Color::rgb(0xff0000)}}));
    dl.roundedRect({330, 10, 140, 60}, Color::rgb(0x334455, 0.8f), {12, 4, 12, 4}, Color::rgb(0xffcc00), 3.f);
    dl.shadow({20, 90, 120, 60}, Color{0, 0, 0, 0.6f}, 10.f, 8.f);
    dl.rect({20, 90, 120, 60}, Color::rgb(0xeeeeee), 8.f);
    Path p;
    p.circle({220, 120}, 40).moveTo({280, 90}).lineTo({330, 150}).lineTo({260, 150}).close();
    dl.path(p, Color::rgb(0x44cc66, 0.9f), true, white4, 2.5f);
    dl.line({340, 90}, {470, 150}, Color::rgb(0xffffff, 0.7f), 3.f);
    dl.pushClip({20, 170, 200, 100}, 18.f);
    dl.rect({0, 160, 260, 120}, C::hdr(Color{1.f, 0.5f, 0.2f, 1.f}, hdr ? 2.f : 1.f));
    dl.popClip();
    dl.backdrop({240, 170, 220, 100}, 12.f, {20, 20, 20, 20}, Color::rgb(0xffffff, 0.25f));
    // (sepia rows sum above 1: the float target keeps the brighter result, so the SDR variant uses grayscale)
    dl.pushFilter(hdr ? ColorMatrix::sepia(0.7f) : ColorMatrix::grayscale(0.7f), 4.f, DropShadow{{4, 4}, 6.f, Color{0, 0, 0, 0.5f}});
    dl.rect({30, 290, 120, 80}, Color::rgb(0x2266ff), 10.f);
    dl.rect({70, 310, 120, 60}, white4.withAlpha(0.6f));
    dl.popFilter();
    dl.rect({220, 290, 60, 60}, white4);
    dl.rect({290, 290, 60, 60}, white4.withAlpha(0.5f));
    if (img != ~0u) {
        DrawCmd c;
        c.op = DrawOp::Image;
        c.image = img;
        c.rect = {360, 290, 100, 80};
        dl.push(c);
    }
}
/// 16 x 16 image: 8-bit ramp, hdrPixels at 2.5x of the same colours
inline void buildHdrImage(Image& im) {
    im.resize(16, 16);
    im.hdrPixels.resize(256);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const Color c{f32(x) / 15.f, f32(y) / 15.f, 0.5f, 1.f};
            im.pixels[std::size_t(y * 16 + x)] = packPremultiplied(c);
            im.hdrPixels[std::size_t(y * 16 + x)] = color::packHdrPixel(color::hdr(c, 2.5f));
        }
}
} // namespace fuse::ui::test
