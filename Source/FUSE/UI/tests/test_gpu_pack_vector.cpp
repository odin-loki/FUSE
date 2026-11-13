// Gate fuse_ui_gpu_pack_vector: what fuse_ui_render's packer does with the vector ops (DrawOp::Path, NinePatch,
// PushClipPath / PushLayer groups). The compute kernel has no op for them: packDrawList pre-rasterises each one with
// the CPU reference rasteriser into an image and emits a kernel Image command. This gate runs without a GPU:
//   * the packed stream only contains ops the kernel implements, the tile bins reference them in draw order
//   * a C++ model of the kernel's Image op (copied from ui_draw_list.comp: tile walk, pixel-area test, rounded clip
//     coverage, texel fetch, blend) run over the packed buffers reproduces the CPU rasteriser's frame exactly -
//     for every vector op, under rect and rounded clips, with group opacity, with glyphs and images inside groups,
//     and with a scissor
//   * statistics: bakedOps / bakedPixels, and over-budget ops are dropped and counted in unsupportedOps
//   * re-packing an unchanged frame does not grow any buffer; a list without vector ops packs exactly as before
//   * the effect groups (PushTransform / PushFilter / PushMask; CSS transform, filter, mask) are baked the same way:
//     one image per top-level group, exact against the CPU rasteriser, also scissored, nested and from a styled tree
// The kernel itself (and so the true GPU output) is only exercised by fuse_ui_vk_parity on a machine with Vulkan.
#include "fuse/ui/painter.hpp"
#include "fuse/ui/render/gpu_draw_data.hpp"
#include "test_util.hpp"

#include <cmath>

using namespace fuse::ui;
using namespace fuse::ui::test;
using fuse::ui::render::GpuCmd;
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::kUiTile;
using fuse::ui::render::packDrawList;

namespace {
constexpr u32 kImageOp = 3; // ui_draw_list.comp: kRect 0, kShadow 1, kGlyph 2, kImage 3, kLine 4

// ---- ui_draw_list.comp, the parts an Image command runs through ----
f32 sdRoundRect(f32 px, f32 py, const f32* r, f32 rad) {
    const f32 cx = r[0] + r[2] * 0.5f, cy = r[1] + r[3] * 0.5f, hx = r[2] * 0.5f, hy = r[3] * 0.5f;
    const f32 dx = px - cx, dy = py - cy;
    const f32 rr = std::min(rad, std::min(hx, hy));
    const f32 qx = std::abs(dx) - hx + rr, qy = std::abs(dy) - hy + rr;
    const f32 ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - rr;
}
u32 q8(f32 v) { return u32(std::floor(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f)); }
void blend(u32 dst[4], const f32 s[4]) {
    if (s[3] <= 0.f && s[0] <= 0.f && s[1] <= 0.f && s[2] <= 0.f) return;
    const f32 inv = 1.f - s[3];
    for (int k = 0; k < 4; ++k) dst[k] = q8(s[k] + f32(dst[k]) / 255.f * inv);
}
/// Runs the kernel model over `data`; false when a command the model (= the Image op) does not cover is met.
bool runKernel(const GpuDrawData& data, Image& target) {
    const u32 W = data.push.extent[0], H = data.push.extent[1];
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x) {
            const u32 p = target.pixels[std::size_t(y) * W + x];
            u32 dst[4]{p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, p >> 24};
            const u32 tile = (y / kUiTile) * data.push.tilesX + (x / kUiTile);
            const u32 begin = data.tiles[tile], end = data.tiles[tile + 1];
            const f32 px = f32(x) + 0.5f, py = f32(y) + 0.5f;
            for (u32 k = begin; k < end; ++k) {
                const GpuCmd& c = data.cmds[data.tiles[data.push.tileCount + 1 + k]];
                if ((c.info[0] & 0xFF) != kImageOp) return false;
                const f32 ax0 = std::max(c.rect[0], c.clip[0]), ay0 = std::max(c.rect[1], c.clip[1]);
                const f32 ax1 = std::min(c.rect[0] + c.rect[2], c.clip[0] + c.clip[2]), ay1 = std::min(c.rect[1] + c.rect[3], c.clip[1] + c.clip[3]);
                if (ax1 <= ax0 || ay1 <= ay0) continue;
                if (int(x) < int(std::floor(ax0)) || int(x) >= int(std::ceil(ax1)) || int(y) < int(std::floor(ay0)) || int(y) >= int(std::ceil(ay1))) continue;
                f32 clipCov = 1.f;
                if (c.params[2] > 0.f) clipCov = std::clamp(0.5f - sdRoundRect(px, py, c.clip, c.params[2]), 0.f, 1.f);
                if (clipCov <= 0.f) continue;
                const u32 img = c.info[3];
                if (img >= data.push.imageCount) continue;
                if (data.images[img * 4 + 3] != 0u) return false; // external textures: not in this gate
                const u32 off = data.images[img * 4], w = data.images[img * 4 + 1], h = data.images[img * 4 + 2];
                const int sx = std::clamp(int((px - c.rect[0]) / c.rect[2] * f32(w)), 0, int(w) - 1);
                const int sy = std::clamp(int((py - c.rect[1]) / c.rect[3] * f32(h)), 0, int(h) - 1);
                const u32 s = data.images[off + u32(sy) * w + u32(sx)];
                const f32 kk = clipCov * (c.fill[3] > 0.f ? c.fill[3] : 1.f);
                const f32 src[4]{f32(s & 0xFF) / 255.f * kk, f32((s >> 8) & 0xFF) / 255.f * kk, f32((s >> 16) & 0xFF) / 255.f * kk, f32(s >> 24) / 255.f * kk};
                blend(dst, src);
            }
            target.pixels[std::size_t(y) * W + x] = dst[0] | (dst[1] << 8) | (dst[2] << 16) | (dst[3] << 24);
        }
    return true;
}
struct Diff {
    int maxErr = 0;
    std::size_t differing = 0;
};
Diff compare(const Image& a, const Image& b) {
    Diff d;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        int m = 0;
        for (int s = 0; s < 32; s += 8) m = std::max(m, std::abs(int((a.pixels[i] >> s) & 0xFF) - int((b.pixels[i] >> s) & 0xFF)));
        d.maxErr = std::max(d.maxErr, m);
        d.differing += m ? 1 : 0;
    }
    return d;
}
} // namespace

int main() {
    constexpr u32 W = 320, H = 240;
    UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
    Image photo; // a classic image and a 9-slice source
    photo.resize(16, 16, 0);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            photo.pixels[std::size_t(y) * 16 + std::size_t(x)] = packPremultiplied(Color{f32(x) / 15.f, f32(y) / 15.f, 0.5f, (x + y) % 5 ? 1.f : 0.6f});
    CpuRasterizer raster;
    raster.addImage(&photo);

    const GradientStop stops[3]{{0.f, Color::rgb(0xFFB000)}, {0.5f, Color::rgb(0xFF3D6E, 0.8f)}, {1.f, Color::rgb(0x6A3DFF, 0.2f)}};
    DrawList dl;
    {
        Painter p(dl, &ui);
        // top level: only ops the model covers (images) and vector ops; classic ops live inside the groups
        p.drawImage(0, {0, 0, f32(W), f32(H)});
        p.setBrush(Gradient::linear({10, 10}, {150, 90}, stops));
        p.setPen(Color::rgb(0xFFFFFF, 0.8f), 2.5f);
        Path blob;
        blob.moveTo({20, 20}).cubicTo({80, -10}, {150, 40}, {120, 90}).quadTo({70, 120}, {20, 80}).close();
        p.drawPath(blob);
        StrokeStyle st;
        st.width = 5.f;
        st.cap = LineCap::Round;
        const f32 dash[2]{9.f, 6.f};
        st.setDashes(dash);
        p.setNoBrush();
        p.setPen(Color::rgb(0x3DFDB0), st);
        p.drawEllipse({230, 60}, 60, 40);
        dl.gradientRect({170, 120, 130, 50}, Gradient::conic({235, 145}, 0.3f, stops), {20, 4, 20, 4});
        dl.gradientRect({10.5f, 130.25f, 60, 40}, Gradient::radial({40, 150}, 30, stops));
        p.drawNinePatch(0, {90, 130, 60, 44}, Edges{4, 4, 4, 4});
        p.drawImage(0, {90.5f, 180, 40, 20}, {4, 4, 8, 8}, true);
        // a clip-path group with classic ops, text and an image inside
        p.save();
        Path circle;
        circle.addCircle({60, 205}, 30);
        p.clipPath(circle);
        p.fillRect({20, 170, 80, 70}, Color::rgb(0x202830));
        p.drawImage(0, {40, 190, 30, 30});
        p.drawText("clip", {30, 196, 60, 18}, Color::rgb(0xFFFFFF), 15.f, TextAlignment::Center);
        p.setPen(Color::rgb(0xFF3D6E), 3.f);
        p.drawLine({20, 240}, {100, 170});
        p.restore();
        // a rounded rect clip around vector ops and a nested opacity layer
        dl.pushClip({150.5f, 176, 150, 60}, 18.f);
        p.fillRect({150, 176, 151, 60}, Gradient::linear({150, 0}, {300, 0}, stops));
        p.beginLayer(0.55f, {160, 180, 130, 50});
        p.setNoPen();
        p.setBrush(Color::rgb(0xFF0000));
        p.drawCircle({200, 205}, 22);
        p.setBrush(Color::rgb(0x0000FF));
        p.drawCircle({225, 205}, 22);
        p.drawText("layer", {240, 196, 50, 18}, Color::rgb(0xFFFFFF), 14.f);
        p.save();
        p.clipPath(circle); // empty intersection: a group inside a group that draws nothing
        p.drawRect({0, 0, 400, 400});
        p.restore();
        p.endLayer();
        dl.popClip();
        dl.pushClipRounded({262, 4, 54, 30}, {14, 0, 14, 0});
        p.drawImage(0, {262, 4, 54, 30});
        dl.popClip();
    }
    UI_CHECK(dl.hasVectorOps());

    Image cpu;
    cpu.resize(int(W), int(H), 0xFF202020u);
    raster.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    raster.render(dl, cpu);

    GpuDrawData data;
    packDrawList(dl, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), raster.images(), raster.imageCount());
    std::printf("packed: %zu kernel commands, %u baked ops, %u baked pixels, %u unsupported, image buffer %zu bytes\n",
                data.cmds.size(), data.bakedOps, data.bakedPixels, data.unsupportedOps, data.imageBytes());
    bool onlyKernelOps = true;
    for (const GpuCmd& c : data.cmds) onlyKernelOps = onlyKernelOps && (c.info[0] & 0xFF) <= 4u;
    UI_CHECK(onlyKernelOps);
    UI_CHECK_EQ(data.unsupportedOps, 0u);
    UI_CHECK(data.bakedOps >= 9);
    UI_CHECK(data.push.imageCount == raster.imageCount() + 11u); // one table slot per top-level vector op / group
    UI_CHECK(data.bakedPixels > 0 && data.imageBytes() >= std::size_t(data.bakedPixels) * 4);

    Image gpu;
    gpu.resize(int(W), int(H), 0xFF202020u);
    UI_CHECK(runKernel(data, gpu));
    Diff d = compare(cpu, gpu);
    std::printf("kernel model vs CPU rasteriser: max err %d, differing %zu / %zu\n", d.maxErr, d.differing, cpu.pixels.size());
    UI_CHECK_EQ(d.maxErr, 0);
    { // the frame is not trivially empty: the vector ops changed a good part of it
        Image base;
        base.resize(int(W), int(H), 0xFF202020u);
        DrawList only;
        Painter p(only);
        p.drawImage(0, {0, 0, f32(W), f32(H)});
        raster.render(only, base);
        UI_CHECK(compare(cpu, base).differing > cpu.pixels.size() / 8);
    }
    // scissored (damage) redraw
    {
        const Rect sc{70, 50, 200, 160};
        Image cpuS, gpuS;
        cpuS.resize(int(W), int(H), 0xFF404040u);
        gpuS.resize(int(W), int(H), 0xFF404040u);
        raster.render(dl, cpuS, sc);
        GpuDrawData ds;
        packDrawList(dl, W, H, ds, ui.atlas().pixels(), u32(ui.atlas().size()), raster.images(), raster.imageCount(), sc);
        UI_CHECK(runKernel(ds, gpuS));
        d = compare(cpuS, gpuS);
        std::printf("scissored: max err %d, differing %zu\n", d.maxErr, d.differing);
        UI_CHECK_EQ(d.maxErr, 0);
        UI_CHECK_EQ(gpuS.at(10, 10), 0xFF404040u);
        UI_CHECK(ds.bakedPixels < data.bakedPixels); // only what the scissor shows is pre-rasterised
    }
    // re-packing the same frame: same buffers, no growth
    {
        const std::size_t capImages = data.images.capacity(), capCmds = data.cmds.capacity(), capTiles = data.tiles.capacity();
        const std::size_t capBake = data.bakeScratch.pixels.capacity();
        const std::vector<u32> imagesBefore = data.images;
        packDrawList(dl, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), raster.images(), raster.imageCount());
        UI_CHECK(data.images.capacity() == capImages && data.cmds.capacity() == capCmds && data.tiles.capacity() == capTiles);
        UI_CHECK(data.bakeScratch.pixels.capacity() == capBake);
        UI_CHECK(data.images == imagesBefore); // deterministic
    }
    // over the bake budget: dropped and counted, the rest still drawn
    {
        constexpr u32 BW = 4200, BH = 4100; // > kMaxBakedPixels as one layer
        DrawList big;
        big.pushLayer(0.5f);
        big.rect({0, 0, f32(BW), f32(BH)}, Color::rgb(0xFF0000));
        big.popClip();
        Path small;
        small.addRect({10, 10, 20, 20});
        big.fillPath(small, Color::rgb(0x00FF00));
        GpuDrawData bd;
        packDrawList(big, BW, BH, bd);
        UI_CHECK_EQ(bd.unsupportedOps, 1u);
        UI_CHECK_EQ(bd.bakedOps, 1u);
        UI_CHECK(bd.bakedPixels <= fuse::ui::render::kMaxBakedPixels);
        UI_CHECK_EQ(bd.cmds.size(), std::size_t(1));
    }
    // a list without vector ops: nothing is baked, the image table is the caller's
    {
        UiContext plain({.viewport = {488, 400}, .font = &font()});
        widgets::install(plain);
        buildSample(plain);
        plain.update();
        GpuDrawData pd;
        packDrawList(plain.drawList(), 488, 400, pd, plain.atlas().pixels(), u32(plain.atlas().size()), raster.images(), raster.imageCount());
        UI_CHECK_EQ(pd.bakedOps, 0u);
        UI_CHECK_EQ(pd.unsupportedOps, 0u);
        UI_CHECK_EQ(pd.push.imageCount, raster.imageCount());
        UI_CHECK_EQ(pd.images.size(), std::size_t(raster.imageCount()) * 4 + photo.pixels.size());
    }
    // effect groups: transform (translated and resampled), filter (blur, colour, drop-shadow), mask - each one image
    {
        DrawList fx;
        const GradientStop fade[2]{{0.f, Color{0, 0, 0, 1}}, {1.f, Color{0, 0, 0, 0.1f}}};
        const FilterOp blur{FilterKind::Blur, 3.f, {}, {}};
        const FilterOp chain[2]{{FilterKind::HueRotate, 1.2f, {}, {}}, {FilterKind::DropShadow, 2.f, {6, 5}, Color{0.f, 0.f, 0.f, 0.7f}}};
        {
            Painter p(fx, &ui);
            p.drawImage(0, {0, 0, f32(W), f32(H)});
            auto card = [&](Rect r, bool text = true) {
                p.fillRect(r, Gradient::linear({r.x, r.y}, {r.right(), r.bottom()}, stops));
                p.drawImage(0, {r.x + 6, r.y + 6, 24, 24});
                if (text) p.drawText("fx", {r.x + 34, r.y + 8, 40, 18}, Color::rgb(0xFFFFFF), 15.f);
                p.setPen(Color::rgb(0xFFFFFF), 2.f);
                p.drawLine({r.x, r.bottom()}, {r.right(), r.y});
            };
            // a translation is packed in place: its commands become moved kernel commands (here an image and two
            // baked vector ops; no glyphs, which this gate's kernel model does not run)
            fx.pushTransform(Affine::translation(12.4f, 8.6f), {10, 10, 80, 50});
            card({10, 10, 80, 50}, false);
            fx.popClip();
            fx.pushTransform(Affine::translation(200, 60) * Affine::rotation(0.6f) * Affine::scaling(1.2f, 0.9f) * Affine::translation(-50, -35), {10, 10, 80, 50});
            card({10, 10, 80, 50});
            fx.popClip();
            fx.pushFilter({&blur, 1}, {20, 90, 80, 50});
            card({20, 90, 80, 50});
            fx.popClip();
            fx.pushClip({120, 120, 90, 80}, 12.f); // under a rounded clip: the kernel clips the baked image
            fx.pushFilter(chain, {130, 130, 80, 50});
            card({130, 130, 80, 50});
            fx.popClip();
            fx.popClip();
            fx.pushMask(Gradient::linear({220, 0}, {310, 0}, fade), {220, 150, 90, 60});
            fx.pushTransform(Affine::translation(265, 180) * Affine::rotation(-0.3f) * Affine::translation(-50, -35), {10, 10, 80, 50});
            card({10, 10, 80, 50});
            fx.popClip();
            fx.popClip();
        }
        Image cpuFx, gpuFx;
        cpuFx.resize(int(W), int(H), 0xFF202020u);
        gpuFx.resize(int(W), int(H), 0xFF202020u);
        raster.render(fx, cpuFx);
        GpuDrawData fd;
        packDrawList(fx, W, H, fd, ui.atlas().pixels(), u32(ui.atlas().size()), raster.images(), raster.imageCount());
        bool kernelOnly = true;
        for (const GpuCmd& c : fd.cmds) kernelOnly = kernelOnly && (c.info[0] & 0xFF) <= 4u;
        UI_CHECK(kernelOnly);
        UI_CHECK_EQ(fd.unsupportedOps, 0u);
        UI_CHECK_EQ(fd.bakedOps, 6u); // the two vector ops of the moved card + one per other top-level group
        UI_CHECK(runKernel(fd, gpuFx));
        Diff fdiff = compare(cpuFx, gpuFx);
        std::printf("effect groups, kernel model vs CPU rasteriser: max err %d, differing %zu\n", fdiff.maxErr, fdiff.differing);
        UI_CHECK_EQ(fdiff.maxErr, 0);
        { // the groups changed the frame
            Image base;
            base.resize(int(W), int(H), 0xFF202020u);
            DrawList only;
            Painter p(only);
            p.drawImage(0, {0, 0, f32(W), f32(H)});
            raster.render(only, base);
            UI_CHECK(compare(cpuFx, base).differing > cpuFx.pixels.size() / 10);
        }
        for (const Rect sc : {Rect{60, 40, 180, 120}, Rect{150, 20, 100, 70}, Rect{30, 100, 50, 30}}) { // through the middle of the groups
            Image cpuS, gpuS;
            cpuS.resize(int(W), int(H), 0xFF404040u);
            gpuS.resize(int(W), int(H), 0xFF404040u);
            raster.render(fx, cpuS, sc);
            GpuDrawData ds;
            packDrawList(fx, W, H, ds, ui.atlas().pixels(), u32(ui.atlas().size()), raster.images(), raster.imageCount(), sc);
            UI_CHECK(runKernel(ds, gpuS));
            UI_CHECK_EQ(compare(cpuS, gpuS).maxErr, 0);
            UI_CHECK(ds.bakedPixels < fd.bakedPixels);
        }
        // the same from a styled tree at a fractional render scale: CSS transform / filter / mask / clip-path nodes
        UiContext styled({.viewport = {f32(W), f32(H)}, .font = &font()}); // core classes only: no theme backgrounds
        UI_CHECK(styled.addStyleSheet(".stage { layout: absolute; width: 320px; height: 240px; } "
                                      ".card { width: 90px; height: 50px; background: #d04020; color: #ffffff; padding: 6px; } "
                                      ".a { left: 20px; top: 20px; transform: rotate(20deg) scale(1.1); } "
                                      ".b { left: 150px; top: 20px; filter: blur(2px) drop-shadow(4px 4px 3px #000000); } "
                                      ".c { left: 20px; top: 120px; mask-image: linear-gradient(to right, black, transparent); clip-path: circle(40px); } "
                                      ".d { left: 150px; top: 120px; transform: translate(10px, 5px); filter: grayscale(1); }").has_value());
        const NodeHandle stage = styled.create(styled.registry().find("panel"), styled.root());
        styled.addClass(stage, "stage");
        for (const char* cls : {"a", "b", "c", "d"}) {
            const NodeHandle n = styled.create(styled.registry().find("label"), stage);
            styled.setText(n, "effect");
            styled.addClass(n, "card");
            styled.addClass(n, cls);
        }
        styled.setRenderScale(1.25f);
        styled.update();
        const u32 SW = 400, SH = 300;
        Image cpuUi, gpuUi;
        cpuUi.resize(int(SW), int(SH), 0xFF101418u);
        gpuUi.resize(int(SW), int(SH), 0xFF101418u);
        CpuRasterizer rs;
        rs.setGlyphAtlas(styled.atlas().pixels(), styled.atlas().size());
        rs.render(styled.drawList(), cpuUi);
        GpuDrawData ud;
        packDrawList(styled.drawList(), SW, SH, ud, styled.atlas().pixels(), u32(styled.atlas().size()), rs.images(), rs.imageCount());
        // classic commands (the cards outside groups: none here; the stage background: none) run through the model's
        // Image op only, so every command of this frame has to be a baked group
        UI_CHECK_EQ(ud.bakedOps, 4u);
        UI_CHECK(runKernel(ud, gpuUi));
        fdiff = compare(cpuUi, gpuUi);
        std::printf("styled tree at scale 1.25, kernel model vs CPU rasteriser: max err %d, differing %zu\n", fdiff.maxErr, fdiff.differing);
        UI_CHECK_EQ(fdiff.maxErr, 0);
        int lit = 0;
        for (u32 px : cpuUi.pixels) lit += px != 0xFF101418u;
        UI_CHECK(lit > 8000);
    }
    return finish("fuse_ui_gpu_pack_vector");
}
