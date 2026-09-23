#pragma once

// Portable scratch paths for tests, smoke registries and probes: never hard-code "/tmp".
// On Linux `std::filesystem::temp_directory_path()` is $TMPDIR or "/tmp", so tempPath("x") is
// "/tmp/x" exactly as before; on Windows it resolves under %TEMP%.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace fuse::test {

/// System temp directory ("/tmp" on Linux), forward slashes, no trailing separator.
inline std::string tempDir() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec || dir.empty()) {
        dir = std::filesystem::path(".");
    }
    std::string out = dir.generic_string();
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

/// `<temp dir>/<relative>` with forward slashes, e.g. tempPath("fuse_x/a.bin") == "/tmp/fuse_x/a.bin".
inline std::string tempPath(std::string_view relative) {
    std::string out = tempDir();
    out.push_back('/');
    out.append(relative);
    return out;
}

/// Create and return a fresh, unique directory `<temp dir>/<prefix>_<pid-ish>_<n>` (removing any
/// stale one first). For tests that need isolation from concurrent runs.
inline std::filesystem::path makeUniqueTempDir(std::string_view prefix) {
    static std::atomic<unsigned> counter{0};
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::string name(prefix);
    name += '_';
    name += std::to_string(stamp);
    name += '_';
    name += std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    std::filesystem::path dir = std::filesystem::path(tempPath(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace fuse::test
