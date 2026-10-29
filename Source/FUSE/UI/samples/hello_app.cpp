// FUSE UI sample: a small settings form built with the retained API plus an immediate-mode debug panel,
// driven by scripted input and rendered by the CPU reference rasteriser to a PNG.
//   fuse_ui_hello_app [out.png]
#include <fuse/ui/context.hpp>
#include <fuse/ui/cpu_raster.hpp>
#include <fuse/ui/immediate.hpp>
#include <fuse/ui/test_driver.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <string>

using namespace fuse;
using namespace fuse::ui;

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "fuse_ui_hello.png";
    UiContext ui;
    ui.setViewport({640.f, 400.f});
    ui.styleSheet().parse("Panel#bg { background: #121316; } Label.title { text-scale: 3; color: #ffffff; }");
    ui.invalidateStyle();

    const NodeId bg = ui.create("Panel", ui.root(), "bg");
    ui.layout(bg).grow = 1.f;
    ui.layout(bg).kind = LayoutKind::Row;
    ui.layout(bg).padding = Insets::all(16.f);
    ui.layout(bg).gap = 16.f;

    // Retained form.
    const NodeId window = ui.create("Panel", bg, "settings");
    ui.setClasses(window, "window");
    ui.layout(window).padding = Insets::all(14.f);
    ui.layout(window).gap = 10.f;
    ui.layout(window).grow = 1.f;
    const NodeId title = ui.create("Label", window);
    ui.setClasses(title, "title");
    ui.setText(title, "FUSE UI");
    const NodeId hint = ui.create("Label", window);
    ui.setClasses(hint, "muted");
    ui.setText(hint, "retained widgets, CPU raster");
    const NodeId name = ui.create("LineEdit", window, "name");
    const NodeId vsync = ui.create("CheckBox", window, "vsync");
    ui.setText(vsync, "VSync");
    const NodeId volume = ui.create("Slider", window, "volume");
    ui.setRange(volume, 0.f, 100.f);
    const NodeId progress = ui.create("ProgressBar", window, "progress");
    const NodeId buttons = ui.create("Panel", window);
    ui.layout(buttons).kind = LayoutKind::Row;
    ui.layout(buttons).gap = 8.f;
    ui.layout(buttons).justify = Justify::End;
    const NodeId cancel = ui.create("Button", buttons, "cancel");
    ui.setText(cancel, "Cancel");
    const NodeId ok = ui.create("Button", buttons, "ok");
    ui.setText(ok, "Apply");
    const NodeId status = ui.create("Label", window, "status");
    ui.setClasses(status, "muted");
    ui.onClicked(ok, [&](UiContext& c, NodeId) {
        c.setText(status, "applied: " + c.text(name) + (c.checked(vsync) ? ", vsync" : "") + ", vol " +
                              std::to_string(int(c.value(volume))));
    });

    // Immediate-mode panel on the same core.
    const NodeId debug = ui.create("Panel", bg, "debug");
    ui.setClasses(debug, "window");
    ui.layout(debug).padding = Insets::all(14.f);
    ui.layout(debug).width = 240.f;
    im::ImmediateUi gui(ui, debug);
    f32 exposure = 1.5f;
    bool wire = true;
    int reloads = 0;
    auto imFrame = [&] {
        gui.begin();
        gui.label("Debug (immediate)");
        gui.slider("Exposure", exposure, -4.f, 4.f);
        gui.checkbox("Wireframe", wire);
        if (gui.button("Reload shaders")) ++reloads;
        gui.label("reloads: " + std::to_string(reloads));
        gui.end();
    };

    UiTestDriver d(ui);
    imFrame();
    d.frame();
    d.click("name");
    d.type("Odin");
    d.click("vsync");
    const Rect vr = ui.rect(volume);
    d.drag({vr.x, vr.y + 4.f}, {vr.x + vr.w * 0.6f, vr.y + 4.f});
    ui.animateValue(progress, 0.7f, 0.2f);
    d.click("ok");
    d.click(d.centre("Reload shaders"));
    for (int i = 0; i < 20; ++i) {
        imFrame();
        d.frame();
    }
    d.move(d.centre("cancel"));
    d.frame();

    Image img;
    img.resize(640, 400, Color::rgb(0x000000));
    rasterize(ui.drawList(), img);
    if (!stbi_write_png(out.c_str(), int(img.width), int(img.height), 4, img.rgba.data(), int(img.width * 4))) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("%s\nstatus: %s\nwrote %s (%zu draw commands)\n", ui.dumpAccessibility().c_str(), ui.text(status).c_str(),
                out.c_str(), ui.drawList().commands().size());
    return 0;
}
