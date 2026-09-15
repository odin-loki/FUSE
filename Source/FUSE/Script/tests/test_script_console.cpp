#include <fuse/core/init.hpp>
#include <fuse/script/script_console.hpp>
#include <fuse/script/script_console_command_registry.hpp>
#include <fuse/script/script_console_history.hpp>
#include <fuse/script/script_host.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

extern int g_failures;

namespace {

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testBuiltInCommandDispatch() {
    fuse::script::ScriptConsole console;
    expectTrue(console.built_in_command_count() >= 7u, "built-in command stubs registered");

    const auto help = console.execute("help");
    expectTrue(help.ok(), "help command succeeds");
    expectTrue(help.output.find("commands:") != std::string::npos, "help lists commands");
    expectTrue(help.output.find("echo") != std::string::npos, "help includes echo");
    expectTrue(help.output.find("load") != std::string::npos, "help includes load");
    expectTrue(help.output.find("list") != std::string::npos, "help includes list");

    const auto listed = console.execute("list");
    expectTrue(listed.ok(), "list command succeeds");
    expectTrue(listed.output.find("help") != std::string::npos, "list includes help");
    expectTrue(listed.output.find('\n') != std::string::npos, "list emits one command per line");

    const auto echoed = console.execute("echo fuse_script_repl");
    expectTrue(echoed.ok(), "echo command succeeds");
    expectTrue(echoed.output == "fuse_script_repl", "echo returns argument text");
    expectTrue(!console.outputLines().empty(), "execute appends output lines");

    const auto cleared = console.execute("clear");
    expectTrue(cleared.ok(), "clear command succeeds");
    expectTrue(console.outputLines().size() == 1u, "clear leaves status line in output");

    const auto unknown = console.execute("not_a_command");
    expectTrue(!unknown.ok(), "unknown command fails");
    expectTrue(unknown.status == fuse::script::ScriptConsoleCommandStatus::UnknownCommand,
               "unknown command status");
    expectTrue(unknown.output.find("unknown command:") != std::string::npos,
               "unknown command returns error string");
}

void testHistoryBufferAndNavigation() {
    fuse::script::ScriptConsole console;
    console.setHistoryCapacity(3);

    console.execute("echo one");
    console.execute("echo one");
    console.execute("echo two");
    expectTrue(console.historyCount() == 2u, "history skips consecutive duplicates");

    console.execute("echo three");
    console.execute("echo four");
    expectTrue(console.historyCount() == 3u, "history capped at capacity");
    expectTrue(console.historyAt(0) == "echo two", "oldest history entry evicted");

    expectTrue(console.recallHistory(true) == "echo four", "history up recalls newest");
    expectTrue(console.recallHistory(true) == "echo three", "history up recalls older entry");
    expectTrue(console.recallHistory(true) == "echo two", "history up stops at oldest");
    expectTrue(console.recallHistory(true) == "echo two", "history up clamps at oldest");

    expectTrue(console.recallHistory(false) == "echo three", "history down recalls newer entry");
    expectTrue(console.recallHistory(false) == "echo four", "history down recalls newest entry");
    expectTrue(console.recallHistory(false).empty(), "history down past newest returns empty");

    console.resetHistoryNavigation();
    expectTrue(console.historyNavigationCursor() == static_cast<fuse::s32>(console.historyCount()),
               "reset history navigation returns to end");

    const auto history = console.execute("history");
    expectTrue(history.ok(), "history command succeeds");
    expectTrue(history.output.find("echo four") != std::string::npos, "history command lists entries");
    expectTrue(history.output.find("history") != std::string::npos, "history command includes itself");
}

void testHostDispatchLoadAndRun() {
    fuse::script::ScriptHost host;
    host.init();

    fuse::script::ScriptConsole console;
    console.attach(&host);

    const auto backend = console.execute("backend");
    expectTrue(backend.ok(), "backend command succeeds with attached host");
    expectTrue(backend.output.find("backend=") != std::string::npos, "backend reports kind");

    const auto missing = console.execute("load /tmp/fuse_script_console_missing.lua");
    expectTrue(!missing.ok(), "load missing file fails");
    expectTrue(missing.status == fuse::script::ScriptConsoleCommandStatus::BackendUnavailable,
               "load failure surfaces backend status");

    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "fuse_script_console";
    std::filesystem::create_directories(temp_dir);
    const std::filesystem::path script_path = temp_dir / "sample.lua";
    {
        std::ofstream out(script_path);
        out << "function on_start() end\n";
    }

    const auto loaded = console.execute(("load " + script_path.string()).c_str());
    expectTrue(loaded.ok(), "load existing file succeeds");
    expectTrue(host.vm().loaded_chunk_count() >= 1u, "load dispatches to script host");

    const auto ran = console.execute("run return 1");
    expectTrue(ran.ok(), "run dispatches lua source to host");
    expectTrue(host.vm().loaded_chunk_count() >= 2u, "run records chunk on host");

    fuse::script::ScriptConsole detached;
    const auto no_host = detached.execute("backend");
    expectTrue(!no_host.ok(), "backend without host fails");
    expectTrue(no_host.status == fuse::script::ScriptConsoleCommandStatus::BackendUnavailable,
               "detached console reports backend unavailable");

    console.detach();
    host.shutdown();
}

void testCommandRegistryDirect() {
    fuse::script::ScriptConsole console;
    fuse::script::ScriptConsoleCommandRegistry registry;
    int invoke_count = 0;

    expectTrue(!registry.register_built_in(nullptr, {}), "built-in rejects null name");
    expectTrue(!registry.register_custom("noop", {}), "custom rejects null handler");

    expectTrue(registry.register_custom("probe", [&](fuse::script::ScriptConsole& /*repl*/,
                                                    const char* /*args*/) {
                   ++invoke_count;
                   return fuse::script::ScriptConsoleCommandResult{
                       fuse::script::ScriptConsoleCommandStatus::Ok, "probe-ok"};
               }),
               "register custom command on registry");

    expectTrue(registry.has_command("probe"), "registry has custom command");
    expectTrue(registry.is_custom("probe"), "probe is custom");
    expectTrue(!registry.is_built_in("probe"), "probe is not built-in");
    expectTrue(!registry.has_command("missing"), "registry lacks unknown command");

    const auto dispatched = registry.dispatch("probe", console, "");
    expectTrue(dispatched.ok(), "registry dispatch succeeds");
    expectTrue(dispatched.output == "probe-ok", "registry dispatch returns handler output");
    expectTrue(invoke_count == 1, "registry dispatch invokes handler");

    const auto unknown = registry.dispatch("missing", console, "");
    expectTrue(unknown.status == fuse::script::ScriptConsoleCommandStatus::UnknownCommand,
               "registry unknown command status");
    expectTrue(unknown.output.find("unknown command:") != std::string::npos,
               "registry unknown command error string");

    const auto empty = registry.dispatch("", console, "");
    expectTrue(empty.status == fuse::script::ScriptConsoleCommandStatus::InvalidArgument,
               "registry rejects empty command name");
}

void testHistoryBufferRingWrapDirect() {
    fuse::script::ScriptConsoleHistoryBuffer history;
    history.setCapacity(3);

    history.push("alpha");
    history.push("beta");
    history.push("gamma");
    expectTrue(history.count() == 3u, "history reaches capacity");
    expectTrue(history.at(0) == "alpha", "oldest entry at index 0");

    history.push("delta");
    expectTrue(history.count() == 3u, "history stays capped after wrap");
    expectTrue(history.at(0) == "beta", "wrap evicts oldest entry");
    expectTrue(history.at(2) == "delta", "newest entry retained after wrap");

    history.push("epsilon");
    expectTrue(history.at(0) == "gamma", "second wrap evicts prior oldest");
    expectTrue(history.at(2) == "epsilon", "second wrap retains newest");

    history.clear();
    expectTrue(history.count() == 0u, "history clear resets count");
    expectTrue(history.recall(true).empty(), "recall on empty history returns empty");

    history.setCapacity(1);
    history.push("solo");
    history.push("next");
    expectTrue(history.count() == 1u, "capacity-one ring keeps one entry");
    expectTrue(history.at(0) == "next", "capacity-one ring retains newest");
}

void testCustomCommandShadowsBuiltIn() {
    fuse::script::ScriptConsole console;
    expectTrue(console.is_built_in_command("echo"), "echo is a built-in");

    expectTrue(console.register_command("echo", [](fuse::script::ScriptConsole& /*repl*/,
                                                   const char* /*args*/) {
                   return fuse::script::ScriptConsoleCommandResult{
                       fuse::script::ScriptConsoleCommandStatus::Ok, "shadowed"};
               }),
               "custom command can shadow built-in name");

    expectTrue(console.has_command("echo"), "shadowed echo is registered");
    expectTrue(console.is_built_in_command("echo"), "built-in echo metadata remains");
    expectTrue(console.custom_command_count() >= 1u, "custom shadow increments custom count");

    const auto shadowed = console.execute("echo original");
    expectTrue(shadowed.ok(), "shadowed echo dispatch succeeds");
    expectTrue(shadowed.output == "shadowed", "custom handler shadows built-in echo");

    expectTrue(console.unregister_command("echo"), "unregister shadow restores built-in path");
    const auto restored = console.execute("echo restored");
    expectTrue(restored.ok(), "built-in echo restored after unregister");
    expectTrue(restored.output == "restored", "built-in echo returns args again");
}

void testHistoryClearStub() {
    fuse::script::ScriptConsole console;
    console.execute("echo one");
    console.execute("echo two");
    expectTrue(console.historyCount() == 2u, "history has entries before clear");

    const auto cleared = console.execute("history clear");
    expectTrue(cleared.ok(), "history clear stub succeeds");
    expectTrue(cleared.output == "history cleared", "history clear stub message");
    expectTrue(console.historyCount() == 0u, "history clear empties buffer");
}

void testCustomCommandDispatch() {
    fuse::script::ScriptConsole console;
    int invoke_count = 0;

    expectTrue(console.register_command("ping", [&](fuse::script::ScriptConsole& /*repl*/,
                                                  const char* args) {
                   ++invoke_count;
                   return fuse::script::ScriptConsoleCommandResult{
                       fuse::script::ScriptConsoleCommandStatus::Ok,
                       std::string("pong:") + (args != nullptr ? args : "")};
               }),
               "register custom command");

    const auto result = console.execute("ping fuse");
    expectTrue(result.ok(), "custom command dispatch succeeds");
    expectTrue(result.output == "pong:fuse", "custom command receives args");
    expectTrue(invoke_count == 1, "custom command handler invoked once");

    const auto help = console.execute("help");
    expectTrue(help.output.find("ping") != std::string::npos, "help lists custom commands");

    expectTrue(console.unregister_command("ping"), "unregister custom command");
    expectTrue(console.custom_command_count() == 0u, "custom command removed");
    const auto missing = console.execute("ping");
    expectTrue(missing.status == fuse::script::ScriptConsoleCommandStatus::UnknownCommand,
               "unregistered command is unknown");
}

} // namespace

void run_script_console_tests() {
    testBuiltInCommandDispatch();
    testCommandRegistryDirect();
    testHistoryBufferRingWrapDirect();
    testHistoryBufferAndNavigation();
    testHistoryClearStub();
    testHostDispatchLoadAndRun();
    testCustomCommandShadowsBuiltIn();
    testCustomCommandDispatch();
}
