// fuse_ui_script_bind (UI8 gate): the Lua `ui` binding table as a whole.
//   - every function of the table is known to this gate and exercised (a new binding without coverage fails it)
//   - argument errors are Lua errors a script can pcall, the state stays usable; a failing handler does not stop
//     the next one
//   - every signal reaches its Lua handler with its arguments; a handler may destroy its own node
//   - handlers are released with their nodes: 2000 create / on / destroy cycles keep the handler count and the
//     Lua heap flat
//   - steady frames (LuaUi::update with nothing changed) leave the Lua heap unchanged
#include "fuse/ui/script/lua_ui.hpp"
#include "test_util.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <set>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
int luaKb(lua_State* L) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    return lua_gc(L, LUA_GCCOUNT, 0);
}
bool run(script::LuaUi& lua, const char* chunk) {
    std::string err;
    const bool ok = lua.run(chunk, &err);
    if (!ok) std::fprintf(stderr, "lua: %s\n", err.c_str());
    return ok;
}
} // namespace

int main() {
    UiContext ui({.viewport = {480, 360}, .font = &font()});
    widgets::install(ui);
    doc::Model model;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        script::LuaUi lua(L, ui, model);

        // 1. the table: every function is one this gate covers below
        const std::set<std::string> covered = {"find", "text", "setText", "value", "setValue", "checked", "setChecked",
                                               "setDisabled", "focus", "addClass", "removeClass", "css", "create",
                                               "destroy", "on", "set", "get", "load", "loadFile", "refresh"};
        std::set<std::string> found;
        lua_getglobal(L, "ui");
        UI_CHECK(lua_istable(L, -1));
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1)) found.insert(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        for (const std::string& f : found)
            if (!covered.count(f)) {
                std::fprintf(stderr, "ui.%s has no coverage in fuse_ui_script_bind\n", f.c_str());
                UI_CHECK(false);
            }
        for (const std::string& f : covered)
            if (!found.count(f)) {
                std::fprintf(stderr, "ui.%s is missing from the table\n", f.c_str());
                UI_CHECK(false);
            }
        std::printf("ui table: %zu functions, all covered\n", found.size());

        // 2. every function, through a document and through script-built nodes
        UI_CHECK(run(lua, R"lua(
            ui.set("player.health", 30); ui.set("player.maxHealth", 40); ui.set("player.name", "Odin")
            ui.set("audio.volume", 0.5); ui.set("flag", true)
            assert(ui.get("flag") == 1 and ui.get("player.name") == "Odin" and ui.get("nope") == nil)
            calls = {}
            ui.actions.save = function(h, a) calls.save = (calls.save or 0) + 1 end
            root = ui.loadFile(")lua" FUSE_UI_TEST_DATA R"lua(/settings.fuseui")
            assert(root and root == ui.find("settings"))
            assert(ui.text("hp") == "HP 75%")
            local panel = ui.load('{"fuseui":1,"root":{"class":"panel","id":"extra_panel","children":[{"class":"label","id":"lbl","text":"hi"}]}}', root)
            assert(panel == ui.find("extra_panel") and ui.text("lbl") == "hi")
            -- create / text / value / checked / disabled / focus / classes / css
            local b = ui.create("button", panel, "b1"); ui.setText(b, "Press")
            assert(ui.text(b) == "Press" and ui.text("b1") == "Press")
            local cb = ui.create("checkbox", panel, "cb"); ui.setChecked(cb, true); assert(ui.checked(cb) == true)
            local sl = ui.create("slider", panel, "sl"); ui.setValue(sl, 0.25); assert(math.abs(ui.value(sl) - 0.25) < 1e-6)
            local le = ui.create("lineedit", panel, "le"); ui.setText(le, "abc")
            ui.setDisabled(b, true); ui.setDisabled(b, false)
            ui.focus(le); assert(ui.focus() == le)
            ui.addClass(b, "primary"); ui.removeClass(b, "primary")
            assert(ui.css(b, "width", "90px") == true)
            assert(ui.css(b, "no-such-property", "1") == false)
            -- every signal kind
            got = {}
            ui.on(b, "clicked", function(h, a) got.clicked = h end)
            ui.on(cb, "toggled", function(h, a) got.toggled = a.flag end)
            ui.on(sl, "valueChanged", function(h, a) got.value = a.value end)
            ui.on(le, "textChanged", function(h, a) got.text = a.text end)
            ui.on(le, "submitted", function(h, a) got.submitted = a.text end)
            ui.on(b, "focusChanged", function(h, a) got.focus = (got.focus or 0) + 1 end)
            ui.setValue(sl, 0.75, true)
            assert(math.abs(got.value - 0.75) < 1e-6)
            assert(ui.refresh() >= 0)
        )lua"));
        ui.update();
        UiTestDriver drv(ui);
        drv.click("b1");
        drv.click("cb");
        ui.setFocus(ui.find("le"));
        drv.type("xy");
        drv.key(Key::Enter);
        ui.setFocus(ui.find("b1"));
        drv.click("save");
        lua.update();
        UI_CHECK(run(lua, R"lua(
            assert(got.clicked == ui.find("b1"))
            assert(got.toggled == false)                 -- was checked, the click unchecked it
            assert(got.text == "abcxy" and got.submitted == "abcxy", tostring(got.text))
            assert(got.focus and got.focus >= 1)
            assert(calls.save == 1)
        )lua"));

        // 3. errors: Lua errors with messages, pcall-able, state intact; a failing handler does not stop the next
        UI_CHECK(run(lua, R"lua(
            local function fails(f, ...) local ok, e = pcall(f, ...); assert(not ok and type(e) == "string", "no error"); return e end
            assert(fails(ui.create, "nope"):find("unknown class"))
            assert(fails(ui.create, "button", 12345678):find("bad parent"))
            assert(fails(ui.on, "nope", "clicked", function() end):find("no such node"))
            assert(fails(ui.on, "b1", "nope", function() end):find("unknown signal"))
            assert(fails(ui.on, "b1", "clicked", 5))
            assert(fails(ui.setText, "b1"))
            assert(fails(ui.load, '{"fuseui":1,"root":{"class":"nope"}}'):find("unknown class"))
            assert(fails(ui.loadFile, "/definitely/missing.fuseui"):find("cannot read"))
            assert(fails(ui.set, "x", {}))
            assert(ui.text("b1") == "Press")             -- still alive and usable
            order = {}
            ui.on("b1", "clicked", function() order[#order + 1] = 1; error("handler failure (expected)") end)
            ui.on("b1", "clicked", function() order[#order + 1] = 2 end)
        )lua"));
        drv.click("b1");
        UI_CHECK(run(lua, "assert(#order == 2 and order[2] == 2)"));

        // 4. a handler that destroys its own node (and its handlers) mid-dispatch
        UI_CHECK(run(lua, R"lua(
            local t = ui.create("button", ui.find("extra_panel"), "selfkill"); ui.setText(t, "x")
            killed = 0
            ui.on(t, "clicked", function(h) killed = killed + 1; ui.destroy(h) end)
            ui.on(t, "clicked", function(h) killed = killed + 100 end)  -- never runs: the node is gone
        )lua"));
        ui.update();
        drv.click("selfkill");
        UI_CHECK(run(lua, "assert(killed == 1 and ui.find('selfkill') == nil)"));

        // 5. handler lifetime: create / on / destroy cycles stay flat
        lua.update();
        const std::size_t handlers0 = lua.handlerCount();
        UI_CHECK(run(lua, R"lua(
            function churn(n)
                for i = 1, n do
                    local w = ui.create("button", ui.find("extra_panel"), "churn")
                    ui.on(w, "clicked", function() end)
                    ui.on(w, "focusChanged", function() end)
                    ui.destroy(w)
                end
            end
            churn(200)
        )lua"));
        const int kb0 = luaKb(L);
        const std::size_t h0 = lua.handlerCount();
        UI_CHECK(run(lua, "churn(2000)"));
        lua.update();
        const int kb1 = luaKb(L);
        std::printf("churn: handlers %zu -> %zu (before churn %zu), lua heap %d -> %d KB\n", h0, lua.handlerCount(), handlers0,
                    kb0, kb1);
        UI_CHECK_EQ(lua.handlerCount(), handlers0);
        UI_CHECK(kb1 <= kb0 + 4);

        // 6. steady frames: no Lua garbage
        for (int i = 0; i < 10; ++i) { lua.update(); ui.update(); }
        const int s0 = luaKb(L);
        for (int i = 0; i < 300; ++i) { lua.update(); ui.update(); }
        const int s1 = luaKb(L);
        std::printf("steady: 300 frames, lua heap %d -> %d KB\n", s0, s1);
        UI_CHECK_EQ(s1, s0);
    }
    lua_close(L);
    return finish("fuse_ui_script_bind");
}
