#include <fuse/renderer/shader/shader_watch.hpp>
#include <fuse/types.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool writeFile(const std::filesystem::path& path, const char* contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << contents;
    out.flush();
    return static_cast<bool>(out);
}

std::filesystem::path uniqueTempPath() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(::getpid());
#endif
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("fuse_shader_watch_" + std::to_string(pid) + "_" + std::to_string(stamp) + ".glsl");
}

void testNullPathRejected() {
    fuse::renderer::ShaderFileWatch watch;
    expectTrue(!watch.watch(nullptr), "watch rejects null path");
    expectTrue(watch.watchedCount() == 0u, "null watch does not record an entry");
    expectTrue(watch.pollChanged() == 0u, "empty watcher poll is zero");
}

void testPollDetectsRewrite() {
    const std::filesystem::path path = uniqueTempPath();
    expectTrue(writeFile(path, "void main() {}\n"), "temp shader file created");

    const std::string pathUtf8 = path.string();
    fuse::renderer::ShaderFileWatch watch;
    expectTrue(watch.watch(pathUtf8.c_str()), "watch records temp shader path");
    expectTrue(watch.watchedCount() == 1u, "watched count is 1");
    expectTrue(watch.pollChanged() == 0u, "unchanged file reports no change");

    expectTrue(writeFile(path, "void main() { /* hot reload */ }\n"), "temp shader rewritten");
    expectTrue(watch.pollChanged() >= 1u, "rewrite reports at least one change");
    expectTrue(watch.pollChanged() == 0u, "second poll after rewrite is stable");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void testMissingFileDoesNotFailPoll() {
    fuse::renderer::ShaderFileWatch watch;
    expectTrue(watch.watch("fuse_shader_watch_missing_does_not_exist.glsl"),
               "missing path can still be watched");
    expectTrue(watch.pollChanged() == 0u, "missing file poll does not fail");
}

} // namespace

int main() {
    testNullPathRejected();
    testPollDetectsRewrite();
    testMissingFileDoesNotFailPoll();

    if (g_failures == 0) {
        std::printf("fuse_shader_watch: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_shader_watch: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
