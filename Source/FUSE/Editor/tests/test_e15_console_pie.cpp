// E15 part 2 (MP-B6-EDITOR-SCRIPT-PIE / UNI-U6-CON-1) gates, CPU headless:
//  - console Lua REPL on the game thread: `print(1+1)` logs 2, a bare expression echoes, syntax
//    errors are reported, ScriptConsole built-ins (`echo`) and `lua <code>` work, REPL entity edits
//    mark the scene modified
//  - engine command dispatch through the CommandQueue: cvar get / set, stat, new / save / open,
//    play / pause / step / resume / stop
//  - PIE: an entity with a Script component moves while playing (ScriptRuntime + ScriptSystem run in
//    the runtime schedule's Scripts stage), physics contacts reach on_collision, the REPL talks to
//    the PIE VM while playing, Stop restores the edit-time scene
//  - script hot-reload during PIE: an edited module is reloaded into the live runtime and logged
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/console_panel.hpp>
#include <fuse/editor/editor_console_commands.hpp>
#include <fuse/editor/editor_host.hpp>

#include <fuse/config/cvar.hpp>
#include <fuse/core/temp_path.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

using fuse::f32;
using fuse::u32;
using fuse::usize;
namespace ecs = fuse::ecs;
namespace editor = fuse::editor;
namespace fs = std::filesystem;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct Rig {
    editor::EditorHost host;
    editor::ConsolePanel console;

    Rig() { editor::registerEditorConsoleCommands(console, host); }

    /// Run a console line as the UI would, tick the game thread, pull the output back. The log is
    /// cleared first so every check sees only this line's output.
    void run(const char* line, int ticks = 1) {
        console.clear();
        console.executeCommand(line);
        for (int i = 0; i < ticks; ++i) {
            host.gameTick();
        }
        host.drainConsoleOutput(console);
    }

    bool logged(const std::string& text, fuse::log::Level level = fuse::log::Level::Info) const {
        for (const editor::ConsolePanel::LogLine& line : console.lines()) {
            if (line.level == level && line.text == text) {
                return true;
            }
        }
        return false;
    }

    bool loggedContaining(const std::string& text, fuse::log::Level level) const {
        for (const editor::ConsolePanel::LogLine& line : console.lines()) {
            if (line.level == level && line.text.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

void writeFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// ---- REPL + commands ------------------------------------------------------------------------------

void testRepl() {
    Rig rig;
    rig.run("print(1+1)");
    expectTrue(rig.logged("2"), "repl: print(1+1) logs 2");
    rig.run("1 + 2");
    expectTrue(rig.logged("3"), "repl: bare expression echoes its value");
    rig.run("x = 40");
    rig.run("print(x + 2)");
    expectTrue(rig.logged("42"), "repl: globals persist across lines");
    rig.run("print(\"a b\")");
    expectTrue(rig.logged("a b"), "repl: quoted strings reach Lua intact");
    rig.run("lua print('via lua')");
    expectTrue(rig.logged("via lua"), "repl: lua <code> command");
    rig.run("echo hello console");
    expectTrue(rig.loggedContaining("hello console", fuse::log::Level::Info), "repl: ScriptConsole built-in (echo)");
    rig.run("this is not lua");
    expectTrue(rig.loggedContaining("console", fuse::log::Level::Error), "repl: syntax error reported as an error");
    rig.run("error('boom')");
    expectTrue(rig.loggedContaining("boom", fuse::log::Level::Error), "repl: runtime error reported");

    // REPL entity edits land in the edit registry and dirty the scene.
    const usize before = rig.host.editorScene().registry().count();
    expectTrue(!rig.host.isSceneDirty(), "repl: scene clean before the edit");
    rig.run("e = Entity.create('from_repl')");
    expectTrue(rig.host.editorScene().registry().count() == before + 1u, "repl: Entity.create in the edit registry");
    expectTrue(rig.host.isSceneDirty(), "repl: entity edit marks the scene modified");
    expectTrue(rig.host.consoleLinesExecuted() >= 9u, "repl: lines ran on the game thread");
}

void testCommands(const fs::path& dir) {
    Rig rig;
    fuse::config::CVarRegistry& cvars = fuse::config::CVarRegistry::global();
    cvars.register_int("e15.test_level", 3, 0, 10, "E15 console gate cvar");
    rig.run("cvar e15.test_level");
    expectTrue(rig.logged("e15.test_level = 3"), "cmd: cvar get");
    rig.run("cvar e15.test_level 7");
    expectTrue(rig.logged("e15.test_level = 7") && cvars.find("e15.test_level")->value_text() == "7", "cmd: cvar set");
    rig.run("cvar e15.test_level 99");
    expectTrue(rig.loggedContaining("cvar e15.test_level", fuse::log::Level::Error) &&
                   cvars.find("e15.test_level")->value_text() == "7",
               "cmd: out-of-range cvar set rejected");
    rig.run("cvar no.such.cvar");
    expectTrue(rig.loggedContaining("unknown cvar", fuse::log::Level::Error), "cmd: unknown cvar");
    rig.run("print(CVar.get('e15.test_level'))");
    expectTrue(rig.logged("7"), "cmd: Lua CVar API sees the console value");

    rig.run("stat");
    expectTrue(rig.loggedContaining("ECS entities", fuse::log::Level::Info) &&
                   rig.loggedContaining("PIE stopped", fuse::log::Level::Info),
               "cmd: stat");

    rig.run("help");
    expectTrue(rig.loggedContaining("cvar", fuse::log::Level::Info) && rig.loggedContaining("play", fuse::log::Level::Info),
               "cmd: help lists the engine commands");

    // Scene commands.
    rig.run("new 2d console_level");
    expectTrue(rig.host.lastFileResult().ok && rig.host.currentSceneDimension() == fuse::scene::SceneDimension::World2D,
               "cmd: new 2d");
    rig.run("e = Entity.create('saved_entity')");
    const std::string path = (dir / "console level.fuselevel").string();
    rig.run(("save \"" + path + "\"").c_str());
    expectTrue(rig.host.lastFileResult().ok && fs::exists(path) && !rig.host.isSceneDirty(), "cmd: save <path>");
    rig.run("new");
    expectTrue(rig.host.editorScene().registry().count() == 0u, "cmd: new clears the scene");
    rig.run(("open \"" + path + "\"").c_str());
    expectTrue(rig.host.lastFileResult().ok && rig.host.editorScene().registry().count() == 1u &&
                   rig.host.currentSceneDimension() == fuse::scene::SceneDimension::World2D,
               "cmd: open <path>");
    rig.run("open");
    expectTrue(rig.loggedContaining("usage: open", fuse::log::Level::Error), "cmd: open usage error");

    // Transport.
    rig.run("play");
    expectTrue(rig.host.editorState().playing && !rig.host.editorState().paused, "cmd: play");
    rig.run("pause");
    expectTrue(rig.host.editorState().paused, "cmd: pause");
    const u32 steps = rig.host.playSession().sessionTickCount();
    rig.run("step");
    expectTrue(rig.host.playSession().manualStepCount() == 1u && rig.host.playSession().sessionTickCount() == steps + 1u,
               "cmd: step advances one step while paused");
    rig.run("resume");
    expectTrue(rig.host.editorState().playing && !rig.host.editorState().paused, "cmd: resume");
    rig.run("step");
    expectTrue(rig.loggedContaining("not paused", fuse::log::Level::Warn), "cmd: step while running is refused");
    rig.run("stop");
    expectTrue(!rig.host.editorState().playing, "cmd: stop");
}

// ---- PIE scripts ----------------------------------------------------------------------------------

constexpr const char* kMover = R"lua(
function on_start(self)
    _G.pie_started = (_G.pie_started or 0) + 1
end
function on_update(self, dt)
    local p = Entity.get_position(self)
    Entity.set_position(self, {x = p.x + self.speed * dt, y = p.y, z = p.z})
    _G.version = 1
end
function on_collision(self, other, point)
    _G.hits = (_G.hits or 0) + 1
end
)lua";

constexpr const char* kMoverV2 = R"lua(
function on_start(self)
    _G.pie_started = (_G.pie_started or 0) + 1
end
function on_update(self, dt)
    _G.version = 2
end
function on_collision(self, other, point)
    _G.hits = (_G.hits or 0) + 1
end
)lua";

void testPieScripts(const fs::path& dir) {
    const fs::path script = dir / "mover.lua";
    writeFile(script, kMover);

    Rig rig;
    rig.run("new");
    ecs::Registry& registry = rig.host.editorScene().registry();
    const ecs::EntityID mover = registry.create();
    ecs::Transform t{};
    t.position = {1.f, 10.f, 0.f, 1.f};
    registry.add(mover, t);
    ecs::Script s{};
    (void)s.set_path(script.string());
    (void)s.set_number("speed", 3.0);
    registry.add(mover, s);

    // A scripted dynamic sphere resting in a static box: contacts every step.
    const ecs::EntityID ball = registry.create();
    ecs::Transform ballT{};
    ballT.position = {20.f, 0.9f, 0.f, 1.f};
    registry.add(ball, ballT);
    ecs::Collider sphere{};
    sphere.shape = ecs::Collider::Sphere;
    sphere.params = {0.5f, 0.f, 0.f, 0.f};
    registry.add(ball, sphere);
    registry.add(ball, ecs::RigidBody{});
    ecs::Script ballScript{};
    (void)ballScript.set_path(script.string());
    (void)ballScript.set_number("speed", 0.0);
    registry.add(ball, ballScript);
    const ecs::EntityID floor = registry.create();
    ecs::Transform floorT{};
    floorT.position = {20.f, 0.f, 0.f, 1.f};
    registry.add(floor, floorT);
    ecs::Collider box{};
    box.shape = ecs::Collider::Box;
    box.params = {2.f, 0.5f, 2.f, 0.f};
    registry.add(floor, box);
    ecs::RigidBody staticBody{};
    staticBody.is_static = true;
    staticBody.mass = 0.f;
    staticBody.inv_mass = 0.f;
    registry.add(floor, staticBody);

    const f32 x0 = registry.get<ecs::Transform>(mover)->position.x;
    rig.run("play", 30);
    expectTrue(rig.host.playSession().scriptsLive(), "pie: script runtime live while playing");
    expectTrue(rig.host.playSession().scriptAttachedCount() == 2u, "pie: both behaviours attached");
    if (!rig.host.playSession().scriptLastError().empty()) {
        std::fprintf(stderr, "pie script error: %s\n", rig.host.playSession().scriptLastError().c_str());
    }
    const f32 x1 = registry.get<ecs::Transform>(mover)->position.x;
    // 30 ticks at 1/60 s, 3 units/s -> ~1.5 units (the first tick runs on_start + on_update).
    expectTrue(x1 > x0 + 1.2f && x1 < x0 + 1.8f, "pie: scripted entity moves during play");
    const ecs::Script* live = registry.get<ecs::Script>(mover);
    expectTrue(live != nullptr && live->started, "pie: component marked started");

    // The REPL talks to the PIE VM while playing.
    rig.run("print(_G.pie_started)");
    expectTrue(rig.logged("2"), "pie: REPL runs in the PIE VM (on_start ran for both behaviours)");
    rig.run("print((_G.hits or 0) > 0)");
    expectTrue(rig.logged("true"), "pie: physics contacts reach on_collision");
    expectTrue(rig.host.playSession().scriptContactDispatchCount() > 0u, "pie: contact dispatch counted");

    // Hot reload: rewrite the module, make sure the mtime moves, let the poll run.
    writeFile(script, kMoverV2);
    std::error_code ec;
    fs::last_write_time(script, fs::last_write_time(script, ec) + std::chrono::seconds(2), ec);
    rig.run("stat", 40);
    expectTrue(rig.host.scriptHotReloadCount() == 1u, "pie: edited module hot-reloaded");
    expectTrue(rig.loggedContaining("script hot-reload: 1 module(s) reloaded", fuse::log::Level::Info),
               "pie: hot-reload logged on the console");
    rig.run("print(_G.version)", 2);
    expectTrue(rig.logged("2"), "pie: running behaviours use the reloaded module");
    const f32 x2 = registry.get<ecs::Transform>(mover)->position.x;
    rig.run("stat", 10);
    expectTrue(registry.get<ecs::Transform>(mover)->position.x == x2, "pie: reloaded on_update no longer moves");

    rig.run("stop");
    expectTrue(!rig.host.playSession().scriptsLive(), "pie: script runtime torn down on stop");
    expectTrue(registry.get<ecs::Transform>(mover)->position.x == x0, "pie: stop restores the entity position");
    const ecs::Script* restored = registry.get<ecs::Script>(mover);
    expectTrue(restored != nullptr && !restored->started && restored->lua_ref == ecs::Script::kNoRef,
               "pie: stop restores the Script component's edit-time state");
    expectTrue(registry.get<ecs::Transform>(ball)->position.y == 0.9f, "pie: stop restores the physics body");

    // Outside play the REPL is back on the editor VM (PIE globals are gone).
    rig.run("print(_G.pie_started)");
    expectTrue(rig.logged("nil"), "pie: editor VM after stop");

    // Play again: a fresh runtime (on_start runs again from a clean VM).
    writeFile(script, kMover);
    rig.run("play", 3);
    rig.run("print(_G.pie_started)");
    expectTrue(rig.logged("2") && rig.host.playSession().scriptsLive(), "pie: second session starts clean");
    rig.run("stop");

    // PIE without Script components does not create a runtime.
    Rig plain;
    plain.run("new");
    plain.host.editorScene().registry().add(plain.host.editorScene().registry().create(), ecs::Transform{});
    plain.run("play", 3);
    expectTrue(!plain.host.playSession().scriptsLive() && plain.host.editorState().playing,
               "pie: no scripts -> no script runtime");
    plain.run("stop");
}

} // namespace

int main() {
    const fs::path dir = fuse::test::makeUniqueTempDir("fuse_e15_console_pie");
    testRepl();
    testCommands(dir);
    testPieScripts(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_editor_e15_console_pie: all checks passed\n");
    return 0;
}
