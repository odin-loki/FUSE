// Gate fuse_ui_doc_animate: document bindings for the Web Animations API and sibling order. `"animate"` (an object or
// an array) takes keyframes as CSS text or an array of keyframe objects (offset / easing / composite + CSS
// properties) and the Animation options (duration, delay, iterations "infinite", direction, fill, easing, composite,
// id, autoplay, timeline "scroll(#id y)" / "view(...)"); `"on": {"finish": ...}` reaches the action callback.
// `"order"` (constant or {binding}) sorts siblings stably; a model change reorders. Cooked documents behave the same
// (the keyframe text is not taken for a binding) and reload restarts the document's animations.
#include "fuse/ui/doc/cook.hpp"
#include "fuse/ui/doc/document.hpp"
#include "test_util.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::doc;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
constexpr const char* kDoc = R"J({"fuseui": 1, "root": {"class": "panel", "id": "root", "children": [
  {"class": "panel", "id": "a", "order": 2, "style": {"height": "10px", "flex-shrink": "0"}},
  {"class": "panel", "id": "b", "order": "{ob}", "style": {"height": "10px", "flex-shrink": "0"}},
  {"class": "panel", "id": "c", "style": {"height": "10px", "flex-shrink": "0"},
   "animate": {"keyframes": "from { opacity: 0 } to { opacity: 1 }", "duration": 1, "id": "fade"},
   "on": {"finish": "faded"}},
  {"class": "panel", "id": "d", "style": {"height": "10px", "flex-shrink": "0"},
   "animate": [{"keyframes": [{"width": "0px"}, {"width": "100px", "offset": 1}], "duration": 2, "fill": "forwards",
                "easing": "linear", "iterations": 1},
               {"keyframes": [{"transform": "scale(3)", "composite": "add"}, {"transform": "scale(3)"}],
                "duration": 1, "fill": "both", "composite": "add", "iterations": "infinite", "autoplay": false}]},
  {"class": "panel", "id": "sc", "style": {"height": "100px", "overflow": "scroll", "flex-shrink": "0"}, "children": [
    {"class": "panel", "id": "item", "style": {"height": "500px", "flex-shrink": "0"},
     "animate": {"keyframes": "from { opacity: 0 } to { opacity: 1 }", "timeline": "scroll(#sc block)"}}]}
]}})J";

int run(UiContext& ui, Document& d, Model& model, const char* label) {
    std::vector<std::string> actions;
    d.onAction([&](std::string_view a, NodeHandle, const SignalArgs&) { actions.emplace_back(a); });
    const NodeHandle root = d.root();
    auto ids = [&] {
        std::string s;
        for (NodeHandle c = ui.firstChild(root); c; c = ui.nextSibling(c)) s += std::string(ui.idOf(c)) + ",";
        return s;
    };
    // order: a=2, b={ob}=1, c/d/sc=0 (stable)
    UI_CHECK(ids() == "c,d,sc,b,a,");
    model.set("ob", 3.0);
    d.refresh();
    UI_CHECK(ids() == "c,d,sc,a,b,");
    model.set("ob", -1.0);
    d.refresh();
    UI_CHECK(ids() == "b,c,d,sc,a,");

    const NodeHandle c = ui.find("c"), dn = ui.find("d"), item = ui.find("item"), sc = ui.find("sc");
    UI_CHECK(c && dn && item && sc);
    const auto anims = ui.animations(c);
    UI_CHECK_EQ(anims.size(), std::size_t(1));
    if (!anims.empty()) UI_CHECK(ui.animId(anims[0]) == "fade");
    ui.update(0.f);
    UI_CHECK(near(ui.computed(c).opacity, 0.f));
    ui.update(0.5f);
    UI_CHECK(near(ui.computed(c).opacity, 0.5f));
    UI_CHECK(near(ui.computed(dn).width.value, 25.f));
    // the second animation of d waits (autoplay false); play it: add composite over the identity transform
    std::vector<AnimationHandle> da;
    for (AnimationHandle h : ui.animations(dn)) da.push_back(h);
    UI_CHECK_EQ(da.size(), std::size_t(1)); // idle ones are not listed
    ui.update(0.6f);
    UI_CHECK_EQ(actions.size(), std::size_t(1));
    if (!actions.empty()) UI_CHECK(actions[0] == "faded");
    ui.update(2.f);
    UI_CHECK(near(ui.computed(dn).width.value, 100.f)); // fill forwards
    UI_CHECK_EQ(int(ui.computed(dn).transformCount), 0);
    // scroll timeline by id
    ui.scrollTo(sc, {0.f, 100.f});
    ui.update(0.f);
    UI_CHECK(near(ui.computed(item).opacity, 0.25f));
    if (ui.computed(item).opacity != 0.25f) std::fprintf(stderr, "%s: item opacity %f\n", label, ui.computed(item).opacity);
    return 0;
}
} // namespace

int main() {
    {
        UiContext ui({.viewport = {200, 400}});
        Model model;
        model.set("ob", 1.0);
        auto doc = Document::load(ui, kDoc, &model);
        if (!doc) {
            std::fprintf(stderr, "load: %s: %s\n", doc.error().path.c_str(), doc.error().message.c_str());
            return 1;
        }
        run(ui, **doc, model, "json");
        // reload: the document's animations restart on the kept nodes (no duplicates)
        const NodeHandle c = ui.find("c");
        UI_CHECK((*doc)->reload(kDoc).has_value());
        UI_CHECK(ui.find("c") == c);
        UI_CHECK_EQ(ui.animations(c).size(), std::size_t(1));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(c).opacity, 0.f));
        // errors
        for (const char* bad : {
                 R"({"fuseui":1,"root":{"animate":{"duration":1}}})",
                 R"({"fuseui":1,"root":{"animate":{"keyframes":"to{opacity:1}","duration":"x"}}})",
                 R"({"fuseui":1,"root":{"animate":{"keyframes":[{"nope":"1"}]}}})",
                 R"J({"fuseui":1,"root":{"animate":{"keyframes":"to{opacity:1}","timeline":"scroll(#missing)"}}})J",
                 R"({"fuseui":1,"root":{"order":"x"}})"}) {
            auto e = Document::load(ui, bad, &model);
            UI_CHECK(!e.has_value());
        }
    }
    {
        auto cooked = cookDocument(kDoc);
        UI_CHECK(cooked.has_value());
        if (cooked) {
            UiContext ui({.viewport = {200, 400}});
            Model model;
            model.set("ob", 1.0);
            auto doc = Document::loadCooked(ui, *cooked, &model);
            UI_CHECK(doc.has_value());
            if (doc) run(ui, **doc, model, "cooked");
            else std::fprintf(stderr, "cooked: %s: %s\n", doc.error().path.c_str(), doc.error().message.c_str());
        }
    }
    return finish("fuse_ui_doc_animate");
}
