// fuse_ui_script_bindings: the `ui::` functions of FORGE script — find / text / value / checked, signal handlers,
// model get / set, a .fuseui document whose actions dispatch to ui::action, create / destroy, stale handles, node
// reordering, scripted animations with finish callbacks, and script errors (never C++ exceptions).
#include "fuse/ui/script/forge_ui.hpp"
#include "test_util.hpp"

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {400, 400}, .font = &font()});
    widgets::install(ui);
    doc::Model model;
    forge::script::engine engine;
    {
        script::ForgeUi fs(engine, ui, model);
        std::string err;
        bool ok = fs.run(R"(
            ui::set("player.health", 30); ui::set("player.maxHealth", 40); ui::set("player.name", "Odin");
            ui::set("audio.volume", 0.5);
            int saves = 0;
            ui::action("save", [](auto h, auto args) { saves = saves + 1; ui::set("busy", 1); });
            auto root = ui::loadFile(")" FUSE_UI_TEST_DATA R"(/settings.fuseui");
            assert(root == ui::find("settings"));
            assert(ui::text("hp") == "HP 75%", ui::text("hp"));
            assert(ui::value("quality") == 1);
            int toggles = 0;
            bool last = false;
            ui::on("mute", "toggled", [](auto h, auto a) { toggles = toggles + 1; last = a["flag"]; });
            auto b = ui::create("button", root, "extra");
            ui::setText(b, "Extra"); assert(ui::text("extra") == "Extra");
            ui::addClass(b, "primary"); assert(ui::hasClass(b, "primary")); assert(ui::css(b, "width", "90px"));
            ui::setValue("vol", 0.75, true);       // emit: two-way writes the model
            assert(ui::get("audio.volume") == 0.75);
        )", &err);
        if (!ok) std::fprintf(stderr, "script: %s\n", err.c_str());
        UI_CHECK(ok);
        ui.update();
        UiTestDriver drv(ui);
        drv.click("mute");
        drv.click("save");
        fs.update();
        err.clear();
        ok = fs.run(R"(
            assert(toggles == 1 && last == true);
            assert(saves == 1);
            assert(ui::get("audio.mute") == 1);
            auto e = ui::find("extra"); ui::destroy(e);
            assert(ui::text(e) == nullptr);        // stale handle reads as nullptr
            ui::setText(e, "x");                   // and is ignored by setters
            assert(ui::find("extra") == nullptr);
            ui::set("player.health", 10); assert(ui::refresh() >= 1); assert(ui::text("hp") == "HP 25%");
        )", &err);
        if (!ok) std::fprintf(stderr, "script: %s\n", err.c_str());
        UI_CHECK(ok);
        UI_CHECK(ui.disabled(ui.find("save"))); // busy = 1 from the action

        // reordering: create, permute, sort by a script comparator, index queries
        err.clear();
        ok = fs.run(R"(
            auto box = ui::create("panel", ui::root(), "box");
            for (int i = 0; i < 4; ++i) ui::create("label", box, "n" + std::to_string(i));
            assert(ui::childCount(box) == 4);
            assert(ui::reorder(box, {ui::find("n3"), ui::find("n2"), ui::find("n1"), ui::find("n0")}));
            assert(ui::id(ui::firstChild(box)) == "n3");
            assert(!ui::reorder(box, {ui::find("n3")}));      // not a permutation: unchanged
            ui::sortChildren(box, [](auto a, auto b) { return ui::id(a) < ui::id(b); });
            auto kids = ui::children(box);
            assert(kids.size() == 4 && ui::id(kids[0]) == "n0" && ui::id(kids[3]) == "n3");
            assert(ui::setChildIndex("n0", 3) && ui::childIndex("n0") == 3);
            assert(ui::swap("n1", "n0") && ui::childIndex("n1") == 3);
            assert(ui::insertBefore("n3", "n2") && ui::childIndex("n3") == 1);
            assert(ui::id(ui::parent("n2")) == "box" && ui::nextSibling("n3") == ui::find("n2"));
        )", &err);
        if (!ok) std::fprintf(stderr, "script: %s\n", err.c_str());
        UI_CHECK(ok);

        // scripted animation with options and an onfinish callback
        err.clear();
        ok = fs.run(R"(
            int finished = 0;
            auto an = ui::animate("n2", "from { opacity: 0 } to { opacity: 1 }",
                                  {{"duration", 0.2}, {"easing", "ease-out"}, {"fill", "forwards"}, {"id", "fade"},
                                   {"onfinish", [](auto a) { finished = finished + 1; }}});
            assert(an != nullptr, "an"); assert(ui::animId(an) == "fade", "id"); assert(ui::animPlayState(an) == "running", ui::animPlayState(an));
            assert(ui::animTarget(an) == ui::find("n2"), "target"); assert(ui::animations("n2").size() == 1, std::to_string(ui::animations("n2").size()));
            int ends = 0;
            ui::on("n2", "finish", [](auto h, auto a) { ends = ends + 1; });
            bool threw = false;
            try { ui::animate("n2", "from { opacity: 0 } to { opacity: 1 }", {{"easing", "bogus("}}); }
            catch (const std::exception& e) { threw = true; }
            assert(threw);
        )", &err);
        if (!ok) std::fprintf(stderr, "script: %s\n", err.c_str());
        UI_CHECK(ok);
        for (int i = 0; i < 20; ++i) ui.update(1.f / 60.f);
        err.clear();
        ok = fs.run(R"(assert(finished == 1, std::to_string(finished)); assert(ends == 1); )", &err);
        if (!ok) std::fprintf(stderr, "script: %s\n", err.c_str());
        UI_CHECK(ok);

        // ui::off disconnects and releases the callback
        const std::size_t before = fs.callbackCount();
        UI_CHECK(fs.run(R"(auto c = ui::on("n1", "clicked", [](auto h, auto a) {}); assert(ui::off(c)); assert(!ui::off(c));)", &err));
        UI_CHECK_EQ(fs.callbackCount(), before);

        // effect ops, timelines + ranges, coordinates through transforms
        err.clear();
        ok = fs.run(R"fs(
            auto b = ui::animate("n1", "from { opacity: 0 } to { opacity: 1 }", {{"duration", 1.0}});
            assert(ui::animReplaceState(b) == "active", "replace");
            assert(ui::animSetKeyframes(b, "from { opacity: 0.5 } to { opacity: 1 }"), "kf");
            assert(ui::animSetTarget(b, "n2"), "target"); assert(ui::animTarget(b) == ui::find("n2"), "target2");
            assert(ui::animPersist(b), "persist"); assert(ui::animReplaceState(b) == "persisted", "persisted");
            assert(ui::documentTime() >= 0.0, "doc time");
            ui::animCancel(b);
            auto v = ui::animate("n1", "from { opacity: 0 } to { opacity: 1 }",
                                 {{"timeline", "view(y 10px)"}, {"rangeStart", "entry 0%"}, {"rangeEnd", "exit 100%"}, {"fill", "both"},
                                  {"onremove", [](auto a) {}}});
            assert(v != nullptr, "view anim");
            auto w = ui::animate("n1", "from { opacity: 0 } to { opacity: 1 }", {{"timeline", "scroll(nearest block)"}, {"range", "cover"}});
            assert(w != nullptr, "scroll anim");
            ui::timelineProgress("n1", "view()", "contain 10%", "contain 90%");
            bool threw = false;
            try { ui::animate("n1", "from { opacity: 0 } to { opacity: 1 }", {{"rangeStart", "entry ten"}}); }
            catch (const std::exception& e) { threw = true; }
            assert(threw, "bad range");
            threw = false;
            try { ui::animate("n1", "from { opacity: 0 } to { opacity: 1 }", {{"timeline", "warp()"}}); }
            catch (const std::exception& e) { threw = true; }
            assert(threw, "bad timeline");
            ui::animCancel(v); ui::animCancel(w);
            ui::css("n2", "transform", "translate(10px, 5px) scale(2)");
        )fs", &err);
        if (!ok) std::fprintf(stderr, "script: %s\n", err.c_str());
        UI_CHECK(ok);
        ui.update(1.f / 60.f);
        err.clear();
        ok = fs.run(R"fs(
            auto l = ui::toLocal("n2", 40.0, 30.0);
            assert(l != nullptr && l.size() == 2, "toLocal");
            auto g = ui::fromLocal("n2", l[0], l[1]);
            assert((g[0] - 40.0) * (g[0] - 40.0) + (g[1] - 30.0) * (g[1] - 30.0) < 0.01, "round trip");
            assert(ui::screenMap("n2").size() == 9, "map");
            assert(ui::screenMap("n2")[0] > 1.5, "scaled");
        )fs", &err);
        if (!ok) std::fprintf(stderr, "script: %s\n", err.c_str());
        UI_CHECK(ok);

        // charts (2D + 3D)
        err.clear();
        ok = fs.run(R"fs(
            auto c = ui::chartView(ui::root(), "chart");
            ui::chartSet(c, {{"title", "Sales"}, {"legend", "bottom"}, {"theme", "light"},
                             {"axes", {{{"type", "category"}, {"categories", {"a", "b", "c"}}}, {{"title", "units"}, {"min", 0.0}}}},
                             {"series", {{{"type", "bar"}, {"name", "2025"}, {"y", {1.0, 2.0, 3.0}}},
                                         {{"type", "line"}, {"name", "2026"}, {"x", {0.0, 1.0, 2.0}}, {"y", {2.0, 2.5, 4.0}}, {"color", "#ff0000"}}}}});
            assert(ui::chartSeriesCount(c) == 2, "series");
            assert(ui::chartPointCount(c, 0) == 3, "points");
            ui::chartAppend(c, 1, 3.0, 5.0);
            ui::chartAppend(c, 1, {4.0, 5.0}, {6.0, 7.0});
            assert(ui::chartPointCount(c, 1) == 6, std::to_string(ui::chartPointCount(c, 1)));
            ui::chartSeries(c, 1, {{"capacity", 4}});
            ui::chartAppend(c, 1, 6.0, 8.0);
            assert(ui::chartPointCount(c, 1) == 4, "capacity");
            auto r = ui::chartRange(c, 1);
            assert(r[0] <= 0.0 && r[1] >= 8.0, "range");
            ui::chartZoomTo(c, 1, 1.0, 2.0);
            assert(ui::chartRange(c, 1)[1] == 2.0, "zoomed");
            ui::chartResetZoom(c);
            assert(ui::chartDescribe(c).size() > 0, "describe");
            assert(ui::chartTooltip(c, 0, 1).size() > 0, "tooltip");
            assert(ui::chartHovered(c) == nullptr, "hover");
            assert(ui::chartSelection(c).size() == 0, "selection");
            ui::chartClear(c, 0);
            assert(ui::chartPointCount(c, 0) == 0, "clear");
            bool threw = false;
            try { ui::chartSet(c, {{"series", {{{"type", "nope"}}}}}); } catch (const std::exception& e) { threw = true; }
            assert(threw, "bad type");
            threw = false;
            try { ui::chartSet("n1", {{"title", "x"}}); } catch (const std::exception& e) { threw = true; }
            assert(threw, "not a chart");

            auto s = ui::chart3dView(ui::root(), "surface", "surf");
            ui::chart3dSet(s, {{"rows", 2}, {"cols", 2}, {"values", {0.0, 1.0, 1.0, 2.0}}, {"camera", {{"yaw", 10.0}, {"pitch", 30.0}}},
                               {"axes", {{{"title", "x"}}, {{"title", "y"}, {"max", 3.0}}}}});
            assert(ui::chart3dCamera(s)["yaw"] == 10.0, "yaw");
            assert(ui::chartDescribe(s).size() > 0, "describe3d");
            assert(ui::chart3dHovered(s) == nullptr, "hover3d");
            assert(ui::chart3dSelection(s).size() == 0, "sel3d");
            auto p = ui::chart3dView(ui::root(), "scatter");
            ui::chart3dSet(p, {{"points", {{0.0, 0.0, 0.0}, {1.0, 2.0, 3.0}}}, {"pointColors", {"red", "blue"}}});
            ui::destroy(p);
        )fs", &err);
        if (!ok) std::fprintf(stderr, "script: %s\n", err.c_str());
        UI_CHECK(ok);
        for (int i = 0; i < 3; ++i) ui.update(1.f / 60.f);

        // errors become script errors with a message, never exceptions
        err.clear();
        UI_CHECK(!fs.run(R"(ui::load("{\"fuseui\":1,\"root\":{\"class\":\"nope\"}}");)", &err));
        UI_CHECK(err.find("unknown class") != std::string::npos);
        UI_CHECK(!fs.run(R"(ui::on("nope", "clicked", [](auto h, auto a) {});)", &err));
        UI_CHECK(!fs.run(R"(ui::on("n1", "bogus", [](auto h, auto a) {});)", &err));
        UI_CHECK(err.find("unknown signal") != std::string::npos);
        UI_CHECK(!fs.run(R"(ui::create("nope");)", &err));
        UI_CHECK_EQ(fs.documents().size(), 1u);
    }
    UI_CHECK_EQ(engine.callbacks(), 0u);
    return finish("fuse_ui_script_bindings");
}
