// GAP-CVAR gates: typed cvars, parse / type errors, precedence (default < config < command line <
// runtime), persistence round trip (only archive cvars saved), callbacks once per change,
// read-only / cheat / requires-restart enforcement, concurrent reads during writes (run under
// TSan with FUSE_CORE_ENABLE_TSAN=ON), and the settings JSON reader used by the action map.

#include <fuse/config/cvar.hpp>
#include <fuse/config/engine_cvars.hpp>
#include <fuse/config/json.hpp>
#include <fuse/core/temp_path.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace fuse;
using namespace fuse::config;

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectResult(CVarResult actual, CVarResult expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected '%s', got '%s')\n", message, to_string(expected), to_string(actual));
        ++g_failures;
    }
}

void testRegistrationAndTypes() {
    CVarRegistry reg;
    CVar<bool> b("t.bool", true, "a bool", CVarFlags::None, reg);
    CVar<i32> i("t.int", 5, 0, 10, "an int", CVarFlags::None, reg);
    CVar<f32> f("t.float", 0.5f, 0.f, 1.f, "a float", CVarFlags::None, reg);
    CVar<std::string> s("t.string", "hello", "a string", CVarFlags::None, reg);
    CVarEnum e("t.enum", {"low", "medium", "high"}, 1, "an enum", CVarFlags::None, reg);
    expect(b.valid() && i.valid() && f.valid() && s.valid() && e.valid(), "all five types register");
    expect(reg.count() == 5, "registry holds five cvars");
    expect(b.get() && i.get() == 5 && f.get() == 0.5f && s.get() == "hello" && e.get() == 1, "defaults readable");
    expect(e.get_name() == "medium", "enum value name");
    expect(reg.find("t.int")->type() == CVarType::Int && reg.find("t.int")->description() == "an int", "metadata");
    expect(reg.find("t.float")->has_range() && reg.find("t.float")->range_high() == 1.0, "float range kept");

    // Same name, same type: the existing entry. Other type: refused.
    expect(reg.register_int("t.int", 1, 0, 3, "again") == &i.entry(), "re-registration returns the entry");
    expect(reg.find("t.int")->get_int() == 5, "re-registration keeps the value");
    expect(reg.register_float("t.int", 1.f, 0.f, 2.f, "clash") == nullptr, "type clash refused");
    // Invalid names / defaults.
    expect(reg.register_bool("", false, "x") == nullptr, "empty name refused");
    expect(reg.register_bool("1abc", false, "x") == nullptr, "leading digit refused");
    expect(reg.register_bool("a..b", false, "x") == nullptr, "double dot refused");
    expect(reg.register_bool("a b", false, "x") == nullptr, "space refused");
    expect(reg.register_int("t.badDefault", 11, 0, 10, "x") == nullptr, "default outside range refused");
    expect(reg.register_enum("t.emptyEnum", std::vector<std::string>{}, 0, "x") == nullptr, "empty enum refused");
    expect(reg.register_enum("t.dupEnum", {"a", "A"}, 0, "x") == nullptr, "duplicate enum names refused");

    const std::vector<std::string> completions = reg.complete("t.f");
    expect(completions.size() == 1 && completions[0] == "t.float", "completion by prefix");
}

void testParseAndTypeErrors() {
    CVarRegistry reg;
    CVar<bool> b("p.bool", false, "", CVarFlags::None, reg);
    CVar<i32> i("p.int", 0, -5, 5, "", CVarFlags::None, reg);
    CVar<f32> f("p.float", 1.f, 0.f, 2.f, "", CVarFlags::None, reg);
    CVarEnum e("p.enum", {"off", "on", "auto"}, 0, "", CVarFlags::None, reg);

    expectResult(reg.set("p.bool", "maybe"), CVarResult::ParseError, "bool rejects 'maybe'");
    expectResult(reg.set("p.bool", "ON"), CVarResult::Ok, "bool accepts ON");
    expect(b.get(), "bool is true after ON");
    expectResult(reg.set("p.bool", "yes"), CVarResult::Unchanged, "yes == true: unchanged");
    expectResult(reg.set("p.int", "3x"), CVarResult::ParseError, "int rejects trailing garbage");
    expectResult(reg.set("p.int", "2.5"), CVarResult::ParseError, "int rejects fraction");
    expectResult(reg.set("p.int", ""), CVarResult::ParseError, "int rejects empty");
    expectResult(reg.set("p.int", "6"), CVarResult::OutOfRange, "int above range");
    expectResult(reg.set("p.int", "-6"), CVarResult::OutOfRange, "int below range");
    expectResult(reg.set("p.int", "99999999999999999999"), CVarResult::ParseError, "int overflow");
    expectResult(reg.set("p.int", " -5 "), CVarResult::Ok, "int accepts trimmed -5");
    expect(i.get() == -5, "int value");
    expectResult(reg.set("p.float", "nan"), CVarResult::ParseError, "float rejects nan");
    expectResult(reg.set("p.float", "inf"), CVarResult::ParseError, "float rejects inf");
    expectResult(reg.set("p.float", "2.5"), CVarResult::OutOfRange, "float above range");
    expectResult(reg.set("p.float", "1.25"), CVarResult::Ok, "float accepts 1.25");
    expect(f.get() == 1.25f, "float value");
    expectResult(f.set(std::nanf("")), CVarResult::ParseError, "typed float set rejects NaN");
    expectResult(reg.set("p.enum", "AUTO"), CVarResult::Ok, "enum by name (case-insensitive)");
    expect(e.get() == 2 && e.get_name() == "auto", "enum index + canonical name");
    expectResult(reg.set("p.enum", "1"), CVarResult::Ok, "enum by index");
    expect(e.get_name() == "on", "enum index 1 = on");
    expectResult(reg.set("p.enum", "7"), CVarResult::OutOfRange, "enum index out of range");
    expectResult(reg.set("p.enum", "sideways"), CVarResult::ParseError, "enum unknown name");
    expectResult(reg.set("p.missing", "1"), CVarResult::UnknownName, "unknown cvar");
    expectResult(reg.set_int(b.entry(), 1), CVarResult::TypeMismatch, "typed setter type mismatch");
    expectResult(reg.set_bool(i.entry(), true), CVarResult::TypeMismatch, "typed setter type mismatch (bool)");
    expect(reg.find("p.float")->value_text() == "1.25", "value_text float");
    std::string canonical;
    expectResult(reg.validate(*reg.find("p.bool"), "off", &canonical), CVarResult::Ok, "validate");
    expect(canonical == "false", "validate canonicalises");
}

void testPrecedence() {
    CVarRegistry reg;
    CVar<i32> v("prec.value", 1, 0, 100, "", CVarFlags::Archive, reg);
    expect(v.entry().source() == CVarSource::Default, "starts at default");

    // Config beats default.
    expect(reg.load_config_text("prec.value = 2\n") == 1, "config applied");
    expect(v.get() == 2 && v.entry().source() == CVarSource::ConfigFile, "config value live");
    // Command line beats config.
    const char* argv[] = {"game", "+prec.value", "3"};
    expect(reg.apply_command_line(3, argv) == 1, "command line applied");
    expect(v.get() == 3 && v.entry().source() == CVarSource::CommandLine, "command line value live");
    // A later config reload cannot beat the command line, but its value is kept for saving.
    std::vector<CVarIssue> issues;
    expect(reg.load_config_text("prec.value = 4\n", &issues) == 0, "config after command line not applied");
    expect(issues.empty(), "being overridden is not an issue");
    expect(v.get() == 3, "command line still wins");
    expect(v.entry().archived_text() && *v.entry().archived_text() == "4", "config value remembered");
    expectResult(reg.set(v.entry(), "5", CVarSource::ConfigFile), CVarResult::Overridden, "explicit Overridden");
    // Runtime beats everything.
    expectResult(v.set(6), CVarResult::Ok, "runtime set");
    expect(v.get() == 6 && v.entry().source() == CVarSource::Runtime, "runtime value live");
    const char* argv2[] = {"game", "-set", "prec.value=7"};
    expect(reg.apply_command_line(3, argv2) == 0, "command line after runtime not applied");
    expect(v.get() == 6, "runtime still wins");
    // Reset goes back to default.
    expectResult(reg.reset("prec.value"), CVarResult::Ok, "reset");
    expect(v.get() == 1 && v.entry().source() == CVarSource::Default && !v.entry().archived_text(), "reset state");

    // Values for names registered later: config then command line, in precedence order.
    reg.load_config_text("late.value = 10\nlate.other = 1\n");
    const char* argv3[] = {"game", "-set", "late.value=20"};
    reg.apply_command_line(3, argv3);
    CVar<i32> late("late.value", 0, 0, 100, "", CVarFlags::Archive, reg);
    expect(late.get() == 20 && late.entry().source() == CVarSource::CommandLine, "pending command line wins");
    expect(late.entry().archived_text() && *late.entry().archived_text() == "10", "pending config kept for save");
    const std::string saved = reg.save_config_text();
    expect(saved.find("late.other = 1") != std::string::npos, "unregistered config values are written back");
}

void testCommandLine() {
    CVarRegistry reg;
    CVar<bool> vsync("cl.vsync", true, "", CVarFlags::None, reg);
    CVar<f32> scale("cl.scale", 1.f, 0.25f, 2.f, "", CVarFlags::None, reg);
    CVar<std::string> name("cl.name", "", "", CVarFlags::None, reg);
    CVar<i32> neg("cl.neg", 0, -10, 10, "", CVarFlags::None, reg);
    const char* argv[] = {"game",     "-set",     "cl.vsync=false", "--mode", "fast", "+cl.scale", "0.5", "-set",
                          "novalue", "+cl.neg", "-3",              "+cl.name", "two words", "+cl.scale"};
    std::vector<CVarIssue> issues;
    const usize applied = reg.apply_command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv, &issues);
    expect(applied == 4, "four values applied");
    expect(!vsync.get() && scale.get() == 0.5f && neg.get() == -3 && name.get() == "two words", "command line values");
    expect(issues.size() == 2, "two command-line issues (no '=' and missing value)");
    const char* bad[] = {"game", "+cl.scale", "9"};
    issues.clear();
    reg.apply_command_line(3, bad, &issues);
    expect(issues.size() == 1 && issues[0].result == CVarResult::OutOfRange && issues[0].line == 1,
           "range error reported with argv index");
}

void testConfigText() {
    CVarRegistry reg;
    CVar<std::string> title("ui.title", "x", "", CVarFlags::Archive, reg);
    CVar<i32> width("video.width", 640, 1, 10000, "", CVarFlags::Archive, reg);
    CVar<bool> flag("ui.flag", false, "", CVarFlags::Archive, reg);
    const std::string text = "\xEF\xBB\xBF# comment\n"
                             "; another\n"
                             "// and another\n"
                             "ui.title = \"Hello \\\"World\\\" # not a comment\"  # trailing comment\n"
                             "[video]\n"
                             "width = 1920 ; inline\n"
                             "[]\n"
                             "ui.flag = true\n"
                             "broken line\n"
                             "video.width = wide\n"
                             "ui.title = \"unterminated\n";
    std::vector<CVarIssue> issues;
    const usize applied = reg.load_config_text(text, &issues);
    expect(applied == 3, "three config values applied");
    expect(title.get() == "Hello \"World\" # not a comment", "quoted value with escapes and '#'");
    expect(width.get() == 1920, "[section] prefix applied");
    expect(flag.get(), "empty section resets the prefix");
    expect(issues.size() == 3, "three issues reported");
    if (issues.size() == 3) {
        expect(issues[0].line == 9 && issues[0].result == CVarResult::ParseError, "missing '=' at line 9");
        expect(issues[1].line == 10 && issues[1].name == "video.width" && issues[1].result == CVarResult::ParseError,
               "type error at line 10");
        expect(issues[2].line == 11, "unterminated quote at line 11");
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testPersistenceRoundTrip() {
    const std::filesystem::path dir = test::makeUniqueTempDir("fuse_cvar_roundtrip");
    const std::filesystem::path file = dir / "nested" / "settings.cfg";

    {
        CVarRegistry reg;
        CVar<f32> volume("a.vol", 1.f, 0.f, 1.f, "", CVarFlags::Archive, reg);
        CVar<std::string> upscaler("r.up", "auto", "", CVarFlags::Archive, reg);
        CVarEnum tier("r.t", {"auto", "T0", "T1"}, 0, "", CVarFlags::Archive, reg);
        CVar<bool> transient("dbg.transient", false, "", CVarFlags::None, reg);
        CVar<bool> god("cheat.god", false, "", CVarFlags::Archive | CVarFlags::Cheat, reg);
        CVar<i32> untouched("r.untouched", 3, 0, 9, "", CVarFlags::Archive, reg);
        CVar<i32> cmdOnly("r.cmd", 3, 0, 9, "", CVarFlags::Archive, reg);
        reg.set_cheats_enabled(true);
        expectResult(volume.set(0.25f), CVarResult::Ok, "set volume");
        expectResult(upscaler.set("fsr 1 \"q\""), CVarResult::Ok, "set upscaler with spaces and quotes");
        expectResult(tier.set(2), CVarResult::Ok, "set tier");
        expectResult(transient.set(true), CVarResult::Ok, "set non-archive");
        expectResult(god.set(true), CVarResult::Ok, "set cheat with cheats on");
        const char* argv[] = {"game", "+r.cmd", "7"};
        reg.apply_command_line(3, argv);
        expect(cmdOnly.get() == 7, "command-line override live");
        expect(reg.save_config_file(file), "save_config_file");
    }
    const std::string saved = readFile(file);
    expect(saved.find("a.vol = 0.25") != std::string::npos, "archive float saved");
    expect(saved.find("r.t = T1") != std::string::npos, "archive enum saved by name");
    expect(saved.find("dbg.transient") == std::string::npos, "non-archive not saved");
    expect(saved.find("cheat.god") == std::string::npos, "cheat not saved");
    expect(saved.find("r.untouched") == std::string::npos, "default-valued cvar not saved");
    expect(saved.find("r.cmd") == std::string::npos, "command-line override not saved");
    expect(!std::filesystem::exists(dir / "nested" / "settings.cfg.tmp"), "temp file renamed away");

    {
        CVarRegistry reg;
        CVar<f32> volume("a.vol", 1.f, 0.f, 1.f, "", CVarFlags::Archive, reg);
        CVar<std::string> upscaler("r.up", "auto", "", CVarFlags::Archive, reg);
        CVarEnum tier("r.t", {"auto", "T0", "T1"}, 0, "", CVarFlags::Archive, reg);
        CVar<bool> transient("dbg.transient", false, "", CVarFlags::None, reg);
        CVar<i32> cmdOnly("r.cmd", 3, 0, 9, "", CVarFlags::Archive, reg);
        std::vector<CVarIssue> issues;
        expect(reg.load_config_file(file, &issues), "load_config_file");
        expect(issues.empty(), "saved file loads without issues");
        expect(volume.get() == 0.25f, "round trip float");
        expect(upscaler.get() == "fsr 1 \"q\"", "round trip string with spaces and quotes");
        expect(tier.get() == 2, "round trip enum");
        expect(!transient.get(), "non-archive stays default");
        expect(cmdOnly.get() == 3, "command-line override did not leak into the file");
        expect(volume.entry().source() == CVarSource::ConfigFile, "loaded values have config source");
        // Saving again reproduces the same file.
        expect(reg.save_config_text() == saved, "save(load(save)) is stable");
        expect(reg.load_config_file(dir / "missing.cfg"), "missing file is not an error");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testCallbacks() {
    CVarRegistry reg;
    CVar<f32> scale("cb.scale", 1.f, 0.f, 4.f, "", CVarFlags::None, reg);
    CVar<std::string> text("cb.text", "a", "", CVarFlags::None, reg);
    int fired = 0;
    f32 seen = 0.f;
    const u64 id = scale.on_change([&](const CVarEntry& e) {
        ++fired;
        seen = e.get_float();
    });
    int listened = 0;
    std::string lastName;
    const u64 listener = reg.add_listener([&](const CVarEntry& e) {
        ++listened;
        lastName = e.name();
    });
    expect(id != 0 && listener != 0, "callback ids");
    scale.set(2.f);
    expect(fired == 1 && seen == 2.f, "callback fires on change with the new value");
    scale.set(2.f);
    reg.set("cb.scale", "2.0");
    expect(fired == 1, "same value does not fire");
    reg.load_config_text("cb.scale = 2\n");
    expect(fired == 1, "config value equal to live value does not fire (and is overridden)");
    reg.set("cb.scale", "bogus");
    expect(fired == 1, "rejected set does not fire");
    scale.set(3.f);
    expect(fired == 2, "second change fires once");
    text.set("b");
    expect(listened == 3 && lastName == "cb.text", "listener sees every change");
    expect(reg.remove_callback(id), "remove callback");
    scale.set(1.f);
    expect(fired == 2, "removed callback no longer fires");
    expect(reg.remove_callback(listener), "remove listener");
    expect(!reg.remove_callback(listener), "second removal fails");
    // A callback may set another cvar (re-entrancy).
    CVar<i32> mirror("cb.mirror", 0, 0, 100, "", CVarFlags::None, reg);
    scale.on_change([&](const CVarEntry& e) { mirror.set(static_cast<i32>(e.get_float() * 10.f)); });
    scale.set(2.5f);
    expect(mirror.get() == 25, "callback can set another cvar");
    reg.reset("cb.scale");
    expect(mirror.get() == 10, "reset fires callbacks too");
}

void testProtection() {
    CVarRegistry reg;
    CVar<i32> ro("sys.threads", 4, 1, 64, "", CVarFlags::ReadOnly | CVarFlags::Archive, reg);
    CVar<bool> noclip("cheat.noclip", false, "", CVarFlags::Cheat, reg);
    CVar<std::string> cheatText("cheat.text", "", "", CVarFlags::Cheat, reg);

    expectResult(ro.set(8), CVarResult::ReadOnly, "read-only rejects runtime set");
    std::vector<CVarIssue> issues;
    reg.load_config_text("sys.threads = 8\n", &issues);
    expect(issues.size() == 1 && issues[0].result == CVarResult::ReadOnly, "read-only rejects config file");
    const char* argv[] = {"game", "+sys.threads", "8"};
    expect(reg.apply_command_line(3, argv) == 1 && ro.get() == 8, "read-only accepts the command line");
    expect(reg.save_config_text().find("sys.threads") == std::string::npos, "read-only never saved");

    expectResult(noclip.set(true), CVarResult::CheatProtected, "cheat rejected while cheats are off");
    expectResult(cheatText.set("x"), CVarResult::CheatProtected, "cheat string rejected while cheats are off");
    const char* argv2[] = {"game", "+cheat.noclip", "1"};
    issues.clear();
    reg.apply_command_line(3, argv2, &issues);
    expect(!noclip.get() && issues.size() == 1 && issues[0].result == CVarResult::CheatProtected,
           "cheat rejected from the command line too");
    reg.set_cheats_enabled(true);
    expectResult(noclip.set(true), CVarResult::Ok, "cheat accepted once cheats are on");
    expect(noclip.get(), "cheat value live");

    CVarEnum tier("r.tierX", {"auto", "T0", "T1"}, 0, "", CVarFlags::Archive | CVarFlags::RequiresRestart, reg);
    expectResult(tier.set(1), CVarResult::Ok, "requires-restart applies during startup");
    reg.finish_startup();
    int fired = 0;
    tier.on_change([&](const CVarEntry&) { ++fired; });
    expectResult(tier.set(2), CVarResult::Deferred, "requires-restart deferred after startup");
    expect(tier.get() == 1 && tier.entry().restart_pending() && tier.entry().pending_text() == "T1" && fired == 0,
           "live value kept, pending recorded, no callback");
    expect(reg.save_config_text().find("r.tierX = T1") != std::string::npos, "deferred value is what gets saved");
    expectResult(tier.set(1), CVarResult::Unchanged, "setting back to the live value cancels the pending change");
    expect(!tier.entry().restart_pending(), "pending cleared");
}

void testConcurrentReads() {
    // Writers on the game thread, readers on workers: values are always one of the written ones
    // (no torn floats / strings). Built with FUSE_CORE_ENABLE_TSAN this also proves race freedom.
    CVarRegistry reg;
    CVar<f32> scale("mt.scale", 0.5f, 0.f, 2.f, "", CVarFlags::None, reg);
    CVar<i32> count("mt.count", 0, 0, 1 << 20, "", CVarFlags::None, reg);
    CVar<std::string> label("mt.label", "aaaa", "", CVarFlags::None, reg);
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::atomic<u64> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            i32 lastCount = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const f32 s = scale.get();
                if (!(s == 0.5f || s == 1.5f)) {
                    bad.fetch_add(1);
                }
                const i32 c = count.get();
                if (c < lastCount) {
                    bad.fetch_add(1); // single writer counts up: reads are monotonic
                }
                lastCount = c;
                const std::string l = label.get();
                if (!(l == "aaaa" || l == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")) {
                    bad.fetch_add(1);
                }
                const CVarEntry* e = reg.find("mt.scale"); // lookups race with registration below
                if (e == nullptr) {
                    bad.fetch_add(1);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int i = 1; i <= 20000; ++i) {
        scale.set((i & 1) != 0 ? 1.5f : 0.5f);
        count.set(i);
        label.set((i & 1) != 0 ? "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" : "aaaa");
        if (i % 1000 == 0) {
            reg.register_bool("mt.dyn" + std::to_string(i), false, "registered while readers run");
        }
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& t : readers) {
        t.join();
    }
    expect(bad.load() == 0, "concurrent reads only see written values");
    expect(reads.load() > 0, "readers ran");
    expect(count.get() == 20000, "final value");
}

void testEngineCVars() {
    CVarRegistry reg;
    const EngineCVars cvars = register_engine_cvars(reg);
    expect(reg.find("r.tier") && reg.find("r.upscaler") && reg.find("r.vsync") && reg.find("r.renderScale") &&
               reg.find("a.masterVolume") && reg.find("in.mouseSensitivity"),
           "engine cvars registered");
    expect(cvars.renderTier.get_name() == "auto" && cvars.upscaler.get() == "auto" && cvars.vsync.get() &&
               cvars.renderScale.get() == 1.f && cvars.masterVolume.get() == 1.f && cvars.mouseSensitivity.get() == 1.f,
           "engine cvar defaults");
    expect(cvars.renderTier.entry().has(CVarFlags::RequiresRestart), "r.tier requires restart");
    expect(cvars.renderScale.entry().has(CVarFlags::Archive), "r.renderScale is archived");
    expectResult(reg.set("r.renderScale", "3"), CVarResult::OutOfRange, "r.renderScale range");
    const EngineCVars again = register_engine_cvars(reg);
    expect(&again.vsync.entry() == &cvars.vsync.entry() && reg.count() == 6, "registration is idempotent");
    const EngineCVars& global = engine_cvars();
    expect(global.vsync.valid() && CVarRegistry::global().find("r.vsync") == &global.vsync.entry(),
           "engine_cvars() uses the global registry");
}

void testUserConfigDir() {
#if !defined(_WIN32) && !defined(__APPLE__)
    const char* old = std::getenv("XDG_CONFIG_HOME");
    const std::string saved = old != nullptr ? old : "";
    setenv("XDG_CONFIG_HOME", "/tmp/fuse_xdg_test", 1);
    expect(CVarRegistry::default_config_path("FUSE") == std::filesystem::path("/tmp/fuse_xdg_test/FUSE/settings.cfg"),
           "XDG_CONFIG_HOME honoured");
    unsetenv("XDG_CONFIG_HOME");
    if (std::getenv("HOME") != nullptr) {
        expect(CVarRegistry::user_config_dir("FUSE") ==
                   std::filesystem::path(std::getenv("HOME")) / ".config" / "FUSE",
               "~/.config fallback");
    }
    if (old != nullptr) {
        setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
    }
#else
    expect(!CVarRegistry::user_config_dir("FUSE").empty(), "user config dir resolved");
#endif
}

void testJson() {
    using json::Value;
    Value v;
    std::string error;
    expect(json::parse(R"({"a": [1, 2.5, -3e2, true, false, null], "s": "x\"\\\/\n\u00e9\ud83d\ude00", "o": {}})", v,
                       &error),
           "valid JSON parses");
    const Value* a = v.find("a");
    expect(a != nullptr && a->items().size() == 6 && a->items()[2].as_number() == -300.0 && a->items()[5].is_null(),
           "array values");
    const Value* s = v.find("s");
    expect(s != nullptr && s->as_string() == "x\"\\/\n\xC3\xA9\xF0\x9F\x98\x80", "string escapes and UTF-8");
    Value round;
    expect(json::parse(json::write(v, true), round) && json::write(round, false) == json::write(v, false),
           "write/parse round trip");

    expect(!json::parse("{\"a\": 1,}", v, &error) && error.rfind("1:", 0) == 0, "trailing comma rejected");
    expect(!json::parse("{\n  \"a\": tru\n}", v, &error) && error.rfind("2:", 0) == 0, "error reports line");
    expect(!json::parse("[1] x", v, &error), "trailing garbage rejected");
    expect(!json::parse("\"abc", v, &error), "unterminated string rejected");
    expect(!json::parse("01", v, &error), "leading zero rejected");
    expect(!json::parse("\"\\ud800\"", v, &error), "unpaired surrogate rejected");
    std::string deep(1000, '[');
    expect(!json::parse(deep, v, &error), "excessive nesting rejected");
}

} // namespace

int main() {
    testRegistrationAndTypes();
    testParseAndTypeErrors();
    testPrecedence();
    testCommandLine();
    testConfigText();
    testPersistenceRoundTrip();
    testCallbacks();
    testProtection();
    testConcurrentReads();
    testEngineCVars();
    testUserConfigDir();
    testJson();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core_cvar: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_core_cvar: all checks passed\n");
    return 0;
}
