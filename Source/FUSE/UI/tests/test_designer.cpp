// UI8 designer prototype gate (fuse_ui_designer_roundtrip):
//   - DesignDocument: load -> save -> load is a fixed point, and every member (bindings, "on" actions, items,
//     unknown keys) survives; the saved text loads in fuse_ui_doc
//   - edits: insert / set property / move / remove with undo / redo back to the identical text
//   - Designer UI: canvas click selects (and does not operate the widget), property edits rebuild the live preview,
//     palette inserts, outline follows the selection
// Optional --out=<dir>: designer.png.
#include "test_util.hpp"

#include "fuse/ui/designer/designer.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;
namespace dz = fuse::ui::designer;

namespace {
const char* kDoc = R"({
  "fuseui": 1,
  "style": ".settings { gap: 6px; padding: 12px; } .hp { color: #ffcc66; }",
  "root": {
    "class": "panel", "id": "settings", "classes": ["settings", "card"],
    "style": { "width": 320 },
    "children": [
      { "class": "label", "id": "hp", "classes": ["hp"], "text": "HP {player.health / player.maxHealth * 100:0}%" },
      { "class": "slider", "id": "vol", "min": 0, "max": 1, "value": "{=audio.volume}" },
      { "class": "checkbox", "id": "mute", "text": "Mute", "checked": "{=audio.mute}" },
      { "class": "combobox", "id": "quality", "items": ["Low", "High", "Ultra"], "selected": 1, "x-editor-note": "kept" },
      { "class": "button", "id": "save", "text": "Save", "disabled": "{busy}", "on": { "clicked": "save" } }
    ]
  }
})";
void step(UiContext& ui) { ui.update(1.f / 60.f); }
void click(UiContext& ui, Vec2 p) { ui.pointerMove(p); ui.pointerDown(p); ui.pointerUp(p); step(ui); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
} // namespace

int main(int argc, char** argv) {
    std::string out;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;

    // ---- model round trip
    dz::DesignDocument d;
    std::string err;
    UI_CHECK(d.load(kDoc, &err));
    UI_CHECK_EQ(d.count(), 6u);
    const std::string s1 = d.save();
    dz::DesignDocument d2;
    UI_CHECK(d2.load(s1));
    UI_CHECK_EQ(d2.save(), s1); // fixed point
    for (const char* kept : {"{=audio.volume}", "\"clicked\": \"save\"", "\"Ultra\"", "x-editor-note", "HP {player.health", ".hp { color"})
        UI_CHECK(s1.find(kept) != std::string::npos);
    UI_CHECK_EQ(d.prop(d.findId("quality"), "items"), std::string("Low High Ultra"));
    UI_CHECK_EQ(d.prop(0, "style.width"), std::string("320"));
    {
        UiContext ui{{.viewport = {400, 300}, .font = &font()}};
        w::install(ui);
        doc::Model m;
        auto live = doc::Document::load(ui, d.previewText(), &m);
        UI_CHECK(doc::Document::load(ui, s1, &m).has_value() == false); // x-* keys are editor-only
        if (!live) std::printf("doc error: %s: %s\n", live.error().path.c_str(), live.error().message.c_str());
        UI_CHECK(live.has_value());
    }
    // edits + undo / redo
    const u32 lbl = d.insert(0, "label");
    UI_CHECK(lbl != dz::DesignDocument::kNone && d.node(lbl)->id() == "label1" && d.parentOf(lbl) == 0u);
    UI_CHECK(d.setProp(lbl, "text", [] { doc::JsonValue v; v.type = doc::JsonValue::Type::String; v.string = "Hello"; return v; }()));
    UI_CHECK(d.setProp(lbl, "style.padding", [] { doc::JsonValue v; v.type = doc::JsonValue::Type::String; v.string = "4px"; return v; }()));
    const u32 moved = d.move(lbl, -1);
    UI_CHECK(moved == lbl - 1u && d.node(moved)->id() == "label1");
    UI_CHECK(d.remove(d.findId("mute")));
    UI_CHECK(d.findId("mute") == dz::DesignDocument::kNone);
    UI_CHECK(!d.remove(0)); // root
    int undos = 0;
    while (d.undo()) ++undos;
    UI_CHECK_EQ(undos, 5);
    UI_CHECK_EQ(d.save(), s1);
    for (int i = 0; i < undos; ++i) UI_CHECK(d.redo());
    UI_CHECK(d.findId("mute") == dz::DesignDocument::kNone && d.prop(d.findId("label1"), "text") == "Hello");
    UI_CHECK(!d.load("{ \"root\": [1, 2] }", &err) && !err.empty());
    UI_CHECK(!d.load("{ \"fuseui\": 1, ", &err) && err.find("line") == 0);

    // ---- designer UI
    UiContext ui{{.viewport = {1280, 720}, .font = &font()}};
    ui.setCss(ui.root(), "padding", "0px");
    dz::Designer de(ui);
    UI_CHECK(de.open(kDoc));
    step(ui);
    const u32 saveIdx = de.document().findId("save");
    const NodeHandle saveBtn = de.canvasNode(saveIdx);
    UI_CHECK(ui.alive(saveBtn));
    UI_CHECK(ui.rect(saveBtn).w > 0.f);
    // the canvas is in design mode: clicking the button selects it, it does not click
    int clicked = 0;
    ui.connect(saveBtn, SignalKind::Clicked, [&clicked](UiContext&, NodeHandle, const SignalArgs&) { ++clicked; });
    click(ui, ctr(ui.rect(saveBtn)));
    UI_CHECK_EQ(de.selected(), saveIdx);
    UI_CHECK_EQ(clicked, 0);
    UI_CHECK(ui.hasClass(saveBtn, "dsel"));
    UI_CHECK(u32(ui.value(de.outline())) == saveIdx);
    UI_CHECK(w::propertyCount(ui, de.properties()) >= 10u);
    UI_CHECK_EQ(w::propertyValue(ui, de.properties(), 2), std::string("Save")); // text row
    // a property edit rebuilds the live preview
    UI_CHECK(de.setProperty("text", "Apply"));
    step(ui);
    UI_CHECK_EQ(std::string(ui.text(de.canvasNode(de.document().findId("save")))), std::string("Apply"));
    UI_CHECK(de.setProperty("width", "140px"));
    step(ui);
    UI_CHECK(std::abs(ui.rect(de.canvasNode(de.document().findId("save"))).w - 140.f) < 0.5f);
    // clicking a combobox selects it too (and does not open its popup)
    const NodeHandle combo = de.canvasNode(de.document().findId("quality"));
    const u32 rootKids = ui.childCount(ui.root());
    click(ui, ctr(ui.rect(combo)));
    UI_CHECK_EQ(de.selected(), de.document().findId("quality"));
    UI_CHECK_EQ(ui.childCount(ui.root()), rootKids); // no overlay / popup was created
    // palette: select the root panel and add a slider and a button via the palette list (Enter)
    de.select(0);
    UI_CHECK(de.addWidget("slider") != dz::DesignDocument::kNone);
    const NodeHandle pal = de.palette();
    ui.setFocus(pal);
    ui.setValue(pal, 2.f); // "button"
    ui.keyDown(Key::Enter); ui.keyUp(Key::Enter);
    step(ui);
    UI_CHECK(de.document().findId("button1") != dz::DesignDocument::kNone);
    UI_CHECK(de.document().findId("slider1") != dz::DesignDocument::kNone);
    UI_CHECK(ui.alive(de.canvasNode(de.document().findId("button1"))));
    // undo twice removes both, redo restores
    de.undo(); de.undo();
    UI_CHECK(de.document().findId("button1") == dz::DesignDocument::kNone && de.document().findId("slider1") == dz::DesignDocument::kNone);
    de.redo(); de.redo();
    UI_CHECK(de.document().findId("button1") != dz::DesignDocument::kNone);
    // the saved text is a valid document again and round-trips
    const std::string saved = de.text();
    dz::DesignDocument d3;
    UI_CHECK(d3.load(saved));
    UI_CHECK_EQ(d3.save(), saved);
    UI_CHECK(saved.find("\"text\": \"Apply\"") != std::string::npos);
    // select the edited button for the screenshot
    de.select(de.document().findId("save"));
    step(ui);

    if (!out.empty()) {
        ui.update();
        Image img;
        img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
        CpuRasterizer r;
        r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r.render(ui.drawList(), img);
        const std::string path = out + "/designer.png";
        stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("wrote %s\n", path.c_str());
    }
    return finish("fuse_ui_designer_roundtrip");
}
