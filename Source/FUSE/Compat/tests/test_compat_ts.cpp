#include <fuse/compat/epoch_ms.hpp>
#include <fuse/compat/timestamp_ms.hpp>
#include <fuse/compat/ts.hpp>
#include <fuse/compat/ts_t2d.hpp>
#include <fuse/compat/ts_t3d.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectOutput(fuse::compat::Dialect dialect,
                  std::string_view source,
                  std::string_view expected,
                  const char* message) {
    const fuse::compat::ExecResult result = fuse::compat::eval(dialect, source, "test.cs");
    if (!result.ok) {
        std::fprintf(stderr, "FAIL: %s (eval error: %s)\n", message, result.error.c_str());
        ++g_failures;
        return;
    }
    if (result.output != expected) {
        std::fprintf(stderr, "FAIL: %s (got '%s' expected '%s')\n",
                     message,
                     result.output.c_str(),
                     std::string(expected).c_str());
        ++g_failures;
    }
    expectTrue(result.dialect == dialect, "eval records the requested dialect");
}

void testEpochMsOk() {
    expectTrue(fuse::compat::epoch_ms_ok(0), "epoch_ms_ok(0) is true");
    expectTrue(fuse::compat::epoch_ms_ok(1000000000), "epoch_ms_ok(1000000000) is true");
    expectTrue(!fuse::compat::epoch_ms_ok(-1), "epoch_ms_ok(-1) is false");
    expectTrue(!fuse::compat::epoch_ms_ok(1000000001), "epoch_ms_ok(1000000001) is false");
}

void testTimestampMsOk() {
    expectTrue(fuse::compat::timestamp_ms_ok(0u), "timestamp_ms_ok(0) is true");
    expectTrue(fuse::compat::timestamp_ms_ok(86400000u), "timestamp_ms_ok(86400000) is true");
    expectTrue(!fuse::compat::timestamp_ms_ok(86400001u), "timestamp_ms_ok(86400001) is false");
}

void testEchoHello() {
    expectOutput(fuse::compat::Dialect::T3d, R"(echo("hello");)", "hello", "echo(\"hello\") produces hello");
}

void testLocalArithmetic() {
    expectOutput(fuse::compat::Dialect::T3d,
                 "%x = 2; echo(%x + 3);",
                 "5",
                 "%x = 2; echo(%x + 3); produces 5");
}

void testFunctionCall() {
    expectOutput(fuse::compat::Dialect::T3d,
                 "function add(%a,%b){return %a+%b;} echo(add(2,3));",
                 "5",
                 "function add(%a,%b){return %a+%b;} echo(add(2,3)); produces 5");
}

void testIfElse() {
    expectOutput(fuse::compat::Dialect::T3d,
                 R"(if (0) { echo("a"); } else { echo("b"); })",
                 "b",
                 "if (0) else branch produces b");
}

void testGlobalString() {
    expectOutput(fuse::compat::Dialect::T3d,
                 R"($g = "ok"; echo($g);)",
                 "ok",
                 "$g = \"ok\"; echo($g); produces ok");
}

void testSyntaxError() {
    const fuse::compat::ExecResult result = fuse::compat::eval(
        fuse::compat::Dialect::T3d, "echo(", "bad.cs");
    expectTrue(!result.ok, "syntax error returns ok=false");
    expectTrue(!result.error.empty(), "syntax error includes an error message");
}

void testComments() {
    expectOutput(fuse::compat::Dialect::T3d,
                 "// line\n/* block */\necho(\"hello\");",
                 "hello",
                 "comments are skipped before echo");
}

void testBothDialects() {
    const char* source = R"(echo("hello");)";
    const fuse::compat::ExecResult t3d = fuse::compat::ts_t3d::eval(source, "t3d.cs");
    const fuse::compat::ExecResult t2d = fuse::compat::ts_t2d::eval(source, "t2d.cs");
    expectTrue(t3d.ok && t3d.output == "hello", "t3d dialect executes echo");
    expectTrue(t3d.dialect == fuse::compat::Dialect::T3d, "t3d dialect tag recorded");
    expectTrue(t3d.dialect == fuse::compat::ts_t3d::dialect(), "t3d wrapper dialect matches");
    expectTrue(t2d.ok && t2d.output == "hello", "t2d dialect executes echo");
    expectTrue(t2d.dialect == fuse::compat::Dialect::T2d, "t2d dialect tag recorded");
    expectTrue(t2d.dialect == fuse::compat::ts_t2d::dialect(), "t2d wrapper dialect matches");
}

void testEvalFile() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "fuse_compat_ts_evalfile.cs";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "%x = 2; echo(%x + 3);";
    }
    const fuse::compat::ExecResult result = fuse::compat::evalFile(fuse::compat::Dialect::T2d, path);
    expectTrue(result.ok, "evalFile succeeds for a readable chunk");
    expectTrue(result.output == "5", "evalFile executes file source");
    expectTrue(result.dialect == fuse::compat::Dialect::T2d, "evalFile records dialect");

    const fuse::compat::ExecResult missing = fuse::compat::evalFile(
        fuse::compat::Dialect::T3d, path.parent_path() / "fuse_compat_ts_missing.cs");
    expectTrue(!missing.ok, "evalFile reports missing files");
    expectTrue(!missing.error.empty(), "evalFile missing file has an error message");
}

} // namespace

int main() {
    testEpochMsOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_compat_ts: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testTimestampMsOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_compat_ts: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testEchoHello();
    testLocalArithmetic();
    testFunctionCall();
    testIfElse();
    testGlobalString();
    testSyntaxError();
    testComments();
    testBothDialects();
    testEvalFile();

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_compat_ts: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_compat_ts: all tests passed\n");
    return EXIT_SUCCESS;
}
