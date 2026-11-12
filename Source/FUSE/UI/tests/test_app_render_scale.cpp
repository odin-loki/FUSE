// fuse_app_render_scale: AppWindow render scale (HiDPI), headless presenter.
//   * WindowDesc::dpiScale / AppWindow::setScale: size(), resize() and input stay logical, image() / writePng() are
//     pixelSize() = size x scale
//   * a click at logical coordinates hits at scale 2; the incremental (damage) present equals a full rasterise
//   * scale 1 is the frame of a window that never had a scale
//   * resizePixels (native backends): the frame is exactly the surface's pixels, the logical size pixels / scale
#include "fuse/app/app.hpp"
#include "fuse/ui/doc/document.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace fuse::ui;
namespace app = fuse::app;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
struct Virt {
    double t = 0.0;
    static double now(void* u) { return static_cast<Virt*>(u)->t; }
    static bool pump(void* u, app::Application&, f32 wait) {
        static_cast<Virt*>(u)->t += std::isinf(wait) ? 1.0 : std::min(double(wait), 1.0);
        return true;
    }
};
constexpr const char* kDoc = R"({ "fuseui": 1, "root": { "class": "panel", "id": "p", "children": [
    { "class": "label", "id": "l", "text": "Render scale" }, { "class": "button", "id": "b", "text": "Go" } ] } })";
bool sameRect(Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_render_scale", .loadSettings = false}};
    Virt v;
    a.setClock({&v, &Virt::now});
    a.setEventSource({&v, &Virt::pump});
    auto& w1 = a.createWindow({.width = 200, .height = 100});
    auto& w2 = a.createWindow({.width = 200, .height = 100, .dpiScale = 2.f});
    CHECK(w1.loadText(kDoc));
    CHECK(w2.loadText(kDoc));
    int clicks = 0;
    w2.ui().connect(w2.ui().find("b"), SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    a.step(0.f);

    CHECK(w1.scale() == 1.f && w2.scale() == 2.f && w2.ui().renderScale() == 2.f);
    CHECK(w2.size().w == 200.f && w2.size().h == 100.f);
    CHECK(w2.pixelSize().w == 400.f && w2.pixelSize().h == 200.f);
    CHECK(w1.pixelSize().w == 200.f && w1.pixelSize().h == 100.f);
    CHECK(w1.image().width == 200 && w1.image().height == 100);
    CHECK(w2.image().width == 400 && w2.image().height == 200);
    CHECK(w1.image().hash() != w2.image().hash());
    const u64 scale1 = w1.image().hash(), scale2 = w2.image().hash();
    // logical layout is the same in both windows
    const Rect b1 = w1.ui().rect(w1.ui().find("b")), b2 = w2.ui().rect(w2.ui().find("b"));
    CHECK(sameRect(b1, b2) && !b2.empty());
    CHECK(sameRect(w1.ui().rect(w1.ui().find("l")), w2.ui().rect(w2.ui().find("l"))));
    // the 2x frame is not the 1x frame with doubled pixels (text is rasterised at the device size)
    if (w2.image().width == 400 && w1.image().width == 200) {
        int diff = 0;
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 400; ++x) diff += w2.image().at(x, y) != w1.image().at(x / 2, y / 2);
        std::printf("  pixels differing from a nearest upscale: %d\n", diff);
        CHECK(diff > 200);
    }

    // input is logical: hover the button's logical centre; the repaint is incremental (damage) and correct
    const Vec2 c{b2.x + b2.w * 0.5f, b2.y + b2.h * 0.5f};
    w2.pointerMove(c);
    a.step(0.f);
    CHECK(w2.ui().hovered() == w2.ui().find("b"));
    {
        const u64 incremental = w2.image().hash();
        CHECK(incremental != scale2); // the hover state was painted (into the damaged device rect)
        const u64 full = w2.rasterize({}).hash();
        CHECK(incremental == full);
    }
    w2.click(c);
    a.step(0.f);
    CHECK(clicks == 1);
    w2.click({c.x * 2.f, c.y * 2.f}); // device coordinates miss
    a.step(0.f);
    CHECK(clicks == 1);
    w2.pointerMove({-1.f, -1.f});
    a.step(0.f);
    w2.ui().setFocus({});
    a.step(0.f);
    CHECK(w2.rasterize({}).hash() == scale2);

    // writePng writes the physical image
    {
        const std::string dir = argc > 1 && std::string(argv[1]).rfind("--out=", 0) == 0 ? std::string(argv[1]).substr(6) : ".";
        const std::string path = dir + "/fuse_app_render_scale.png";
        CHECK(w2.writePng(path));
        unsigned char hdr[24] = {};
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            CHECK(std::fread(hdr, 1, sizeof hdr, f) == sizeof hdr);
            std::fclose(f);
        }
        const unsigned pw = unsigned(hdr[16]) << 24 | unsigned(hdr[17]) << 16 | unsigned(hdr[18]) << 8 | hdr[19];
        const unsigned ph = unsigned(hdr[20]) << 24 | unsigned(hdr[21]) << 16 | unsigned(hdr[22]) << 8 | hdr[23];
        CHECK(pw == 400 && ph == 200);
    }

    // setScale both ways reproduces the other window's frame
    w2.setScale(1.f);
    w1.setScale(2.f);
    a.step(0.f);
    CHECK(w2.pixelSize().w == 200.f && w2.image().width == 200 && w2.image().height == 100);
    CHECK(w1.pixelSize().w == 400.f && w1.image().width == 400 && w1.image().height == 200);
    CHECK(w2.image().hash() == scale1);
    CHECK(w1.image().hash() == scale2);
    CHECK(w1.size().w == 200.f && w1.size().h == 100.f);

    // resize is logical
    w1.resize(300, 150);
    a.step(0.f);
    CHECK(w1.size().w == 300.f && w1.size().h == 150.f);
    CHECK(w1.pixelSize().w == 600.f && w1.pixelSize().h == 300.f);
    CHECK(w1.image().width == 600 && w1.image().height == 300);
    CHECK(w1.ui().rect(w1.ui().root()).w == 300.f);

    // fractional scale: a sane frame
    w1.setScale(1.5f);
    a.step(0.f);
    CHECK(w1.image().width == 450 && w1.image().height == 225);
    CHECK(w1.size().w == 300.f);
    {
        int lit = 0;
        for (u32 p : w1.image().pixels) lit += ((p >> 8) & 0xFFu) > 160u;
        CHECK(lit > 100);
    }

    // a native surface of 1080 x 607 px at density 2.625: the frame is exactly the surface, the UI is pixels / scale
    w1.resizePixels(1080, 607, 2.625f);
    a.step(0.f);
    CHECK(w1.scale() == 2.625f);
    CHECK(w1.pixelSize().w == 1080.f && w1.pixelSize().h == 607.f);
    CHECK(w1.image().width == 1080 && w1.image().height == 607);
    CHECK(std::abs(w1.size().w - 1080.f / 2.625f) < 1e-3f && std::abs(w1.size().h - 607.f / 2.625f) < 1e-3f);
    {
        const Rect root = w1.ui().rect(w1.ui().root());
        CHECK(std::abs(root.w * 2.625f - 1080.f) < 0.01f);
        const Rect b = w1.ui().rect(w1.ui().find("b"));
        w1.ui().connect(w1.ui().find("b"), SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
        w1.click({b.x + b.w * 0.5f, b.y + b.h * 0.5f});
        a.step(0.f);
        CHECK(clicks == 2);
    }

    if (g_fail) {
        std::fprintf(stderr, "fuse_app_render_scale: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("fuse_app_render_scale: OK\n");
    return 0;
}
