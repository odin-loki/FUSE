#include <fuse/core/init.hpp>
#include <fuse/script/route_token.hpp>
#include <fuse/script/script_host.hpp>
#include <fuse/script/script_indent.hpp>
#include <fuse/script/script_result.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

int g_failures = 0;

namespace {

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// Indent spaces are the closed range 0..32. No allocation.
void testScriptIndentOk() {
    expectTrue(fuse::script::script_indent_ok(0), "script_indent_ok(0) is true");
    expectTrue(fuse::script::script_indent_ok(32), "script_indent_ok(32) is true");
    expectTrue(!fuse::script::script_indent_ok(-1), "script_indent_ok(-1) is false");
    expectTrue(!fuse::script::script_indent_ok(33), "script_indent_ok(33) is false");
}

// No allocation. A failure returns from main before later tests.
void testRouteTokenOk() {
    expectTrue(fuse::script::route_token_ok("Reload"), "Reload is a valid route token");
    expectTrue(!fuse::script::route_token_ok(""), "empty route token is invalid");
    expectTrue(!fuse::script::route_token_ok("1Reload"), "1Reload is an invalid route token");
    expectTrue(!fuse::script::route_token_ok("Re load"), "Re load is an invalid route token");
}

void testCompatRouteChunks() {
    fuse::script::ScriptHost host;
    expectTrue(host.init(), "script host initializes");

    const auto t3d_ok = host.load_string("echo(\"hello\");", "t3d:hello");
#if defined(FUSE_HAS_COMPAT_TS) && FUSE_HAS_COMPAT_TS
    expectTrue(t3d_ok.ok(), "t3d:hello executes on compat VM");
#else
    expectTrue(!t3d_ok.ok(), "t3d:hello fails when compat VM is not linked");
    expectTrue(t3d_ok.status == fuse::script::ScriptLoadStatus::BackendUnavailable,
               "t3d:hello reports BackendUnavailable without compat VM");
    expectTrue(t3d_ok.message != nullptr && std::string(t3d_ok.message).find("compat") != std::string::npos,
               "t3d:hello explains that the compat VM is not linked");
#endif

    const auto t3d_bad = host.load_string("echo(", "t3d:bad");
#if defined(FUSE_HAS_COMPAT_TS) && FUSE_HAS_COMPAT_TS
    expectTrue(!t3d_bad.ok(), "t3d:bad fails");
    expectTrue(t3d_bad.status == fuse::script::ScriptLoadStatus::ParseError,
               "t3d:bad reports ParseError");
    expectTrue(t3d_bad.message != nullptr && t3d_bad.message[0] != '\0',
               "t3d:bad error message stays valid");
#else
    expectTrue(t3d_bad.status == fuse::script::ScriptLoadStatus::BackendUnavailable,
               "t3d:bad reports BackendUnavailable without compat VM");
#endif

    const auto t2d_ok = host.load_string("echo(\"hello\");", "t2d:hello");
#if defined(FUSE_HAS_COMPAT_TS) && FUSE_HAS_COMPAT_TS
    expectTrue(t2d_ok.ok(), "t2d:hello executes on compat VM");
#else
    expectTrue(!t2d_ok.ok(), "t2d:hello fails when compat VM is not linked");
    expectTrue(t2d_ok.status == fuse::script::ScriptLoadStatus::BackendUnavailable,
               "t2d:hello reports BackendUnavailable without compat VM");
#endif
    expectTrue(host.vm().loaded_chunk_count() == 0u,
               "t3d/t2d chunks are not recorded on the Lua/null VM");

    const auto fuse_chunk = host.load_string("echo(\"hello\");", "fuse:hello");
    expectTrue(fuse_chunk.status != fuse::script::ScriptLoadStatus::BackendUnavailable,
               "fuse:hello uses ScriptVM and does not require the compat VM");

    host.shutdown();
}

} // namespace

int main() {
    const int scriptIndentFailures = g_failures;
    testScriptIndentOk();
    if (g_failures != scriptIndentFailures) {
        std::fprintf(stderr, "fuse_script_compat_route: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    const int routeTokenFailures = g_failures;
    testRouteTokenOk();
    if (g_failures != routeTokenFailures) {
        std::fprintf(stderr, "fuse_script_compat_route: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    fuse::core::initialize();
    testCompatRouteChunks();

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_compat_route: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_script_compat_route: all tests passed\n");
    return EXIT_SUCCESS;
}

#include <fuse/script/route_name.hpp>

namespace {

void testScriptRouteOk() {
    expectTrue(fuse::script::script_route_ok("player_tick"), "player_tick is a valid route name");
    expectTrue(!fuse::script::script_route_ok("1start"), "1start is an invalid route name");
    expectTrue(!fuse::script::script_route_ok(""), "empty route name is invalid");
}

// Runs before main(); failures accumulate in g_failures, which main() already reports.
struct ScriptRouteOkTestRegistration {
    ScriptRouteOkTestRegistration() { testScriptRouteOk(); }
};

[[maybe_unused]] const ScriptRouteOkTestRegistration g_scriptRouteOkTestRegistration{};

} // namespace
