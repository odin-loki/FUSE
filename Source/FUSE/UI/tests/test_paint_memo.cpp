// Gate fuse_ui_paint_memo: the paint memo (UiContext: a clipped subtree unchanged since the previous paint copies that
// paint's commands; repeated text paints reuse their glyph commands) never changes what is drawn. After each kind of
// change the list equals the list of a fresh context built with the same final state, and painted counts match.
#include "test_util.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {

struct Tree {
    UiContext ui;
    std::vector<NodeHandle> panels, items;
    explicit Tree() : ui({.viewport = {640, 360}, .font = &font()}) {
        widgets::install(ui);
        ui.setCss(ui.root(), "layout", "flow");
        ui.setCss(ui.root(), "overflow", "clip");
        char buf[16];
        for (int p = 0; p < 6; ++p) {
            const NodeHandle panel = widgets::panel(ui, ui.root());
            ui.setCss(panel, "width", "200px");
            ui.setCss(panel, "height", "110px");
            ui.setCss(panel, "overflow", "clip");
            ui.setCss(panel, "layout", "flow");
            panels.push_back(panel);
            for (int i = 0; i < 12; ++i) {
                std::snprintf(buf, sizeof buf, "%d.%d", p, i);
                items.push_back((i % 3 == 0) ? widgets::button(ui, panel, buf) : widgets::label(ui, panel, buf));
            }
        }
    }
};

// What a step changes, applied to both the long-lived tree and the fresh reference.
using Step = void (*)(Tree&);

bool sameList(const DrawList& a, const DrawList& b, const char* what) {
    if (a.size() != b.size()) {
        std::fprintf(stderr, "%s: %zu commands, fresh context %zu\n", what, a.size(), b.size());
        return false;
    }
    for (std::size_t k = 0; k < a.size(); ++k) {
        const DrawCmd& x = a.cmds()[k];
        const DrawCmd& y = b.cmds()[k];
        // atlas texels (u, v) depend on the order glyphs entered each context's atlas: compared by size only
        const bool same = x.op == y.op && x.node == y.node && x.rect == y.rect && x.fill.r == y.fill.r &&
                          x.fill.g == y.fill.g && x.fill.b == y.fill.b && x.fill.a == y.fill.a && x.tw == y.tw &&
                          x.th == y.th && x.gradient == y.gradient && x.borderWidth == y.borderWidth;
        if (!same) {
            std::fprintf(stderr, "%s: command %zu differs (op %d / %d, node %u / %u)\n", what, k, int(x.op), int(y.op),
                         x.node, y.node);
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    Tree live;
    live.ui.update();
    live.ui.update();
    const std::vector<std::pair<const char*, Step>> steps = {
        {"setText", [](Tree& t) { t.ui.setText(t.items[14], "changed"); }},
        {"button text", [](Tree& t) { t.ui.setText(t.items[24], "OK!"); }},
        {"hover state", [](Tree& t) { t.ui.setState(t.items[36], kStateHover, true); }},
        {"inline colour", [](Tree& t) { t.ui.setCss(t.items[40], "color", "#ff0000"); }},
        {"panel background", [](Tree& t) { t.ui.setCss(t.panels[4], "background", "#203040"); }},
        {"resize panel", [](Tree& t) { t.ui.setCss(t.panels[1], "width", "150px"); }},
        {"remove item", [](Tree& t) { t.ui.destroy(t.items[50]); }},
        {"style sheet", [](Tree& t) { (void)t.ui.addStyleSheet("label { color: #00ff00; }"); }},
        {"opacity", [](Tree& t) { t.ui.setCss(t.panels[5], "opacity", "0.5"); }},
        {"viewport", [](Tree& t) { t.ui.setViewport({500, 300}); }},
    };
    for (std::size_t s = 0; s < steps.size(); ++s) {
        steps[s].second(live);
        live.ui.update();
        const u32 painted = live.ui.stats().painted;
        Tree fresh;
        for (std::size_t k = 0; k <= s; ++k) steps[k].second(fresh);
        fresh.ui.update();
        UI_CHECK(sameList(live.ui.drawList(), fresh.ui.drawList(), steps[s].first));
        if (painted != fresh.ui.stats().painted)
            std::fprintf(stderr, "%s: painted %u, fresh context %u\n", steps[s].first, painted, fresh.ui.stats().painted);
        UI_CHECK_EQ(painted, fresh.ui.stats().painted);
        // a frame with nothing changed paints nothing new; one more change after it still matches
        live.ui.update();
    }
    return finish("fuse_ui_paint_memo");
}
