// FUSE Relight RL-6.4: fuse_relight_plugins, the plugin discovery report shipped in the package.
//
//   fuse_relight_plugins [--dir <Relight dir>] [--platform windows|linux] [--json] [--require <id>]...
//   fuse_relight_plugins --file-version <file>
//
// Without --dir the directory of this executable is used (it ships next to the Relight d3d9.dll).
// Exit codes: 0 report written (and every --require'd plugin usable), 1 a required plugin is not
// usable, 2 usage error, 3 --file-version found no version resource.
#include <fuse/relight/package/plugin_discovery.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace fuse::relight::package;

namespace {

std::string selfDirectory() {
#if defined(_WIN32)
    char path[4096];
    const DWORD n = ::GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    if (n == 0 || n >= sizeof(path)) {
        return {};
    }
    return std::filesystem::path(std::string(path, n)).parent_path().string();
#else
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.parent_path().string();
#endif
}

int usage() {
    std::fprintf(stderr,
                 "usage: fuse_relight_plugins [--dir <Relight dir>] [--platform windows|linux] [--json] [--require <id>]...\n"
                 "       fuse_relight_plugins --file-version <file>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    DiscoveryConfig config;
    bool json = false;
    bool dirGiven = false;
    std::vector<std::string> required;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--dir" && hasValue) {
            config.moduleDir = argv[++i];
            dirGiven = true;
        } else if (a == "--platform" && hasValue) {
            const std::string p = argv[++i];
            if (p == "windows") {
                config.platform = PluginPlatform::Windows;
            } else if (p == "linux") {
                config.platform = PluginPlatform::Linux;
            } else {
                return usage();
            }
        } else if (a == "--json") {
            json = true;
        } else if (a == "--require" && hasValue) {
            required.emplace_back(argv[++i]);
        } else if (a == "--file-version" && hasValue) {
            FileVersion v;
            if (!readPeFileVersion(argv[i + 1], v)) {
                std::printf("%s: no version resource\n", argv[i + 1]);
                return 3;
            }
            std::printf("%s: %s\n", argv[i + 1], formatVersion(v).c_str());
            return 0;
        } else {
            return usage();
        }
    }
    if (!dirGiven) {
        config.moduleDir = selfDirectory();
    }
    const std::vector<PluginResult> results = discoverPlugins(config);
    const std::string text = json ? formatDiscoveryJson(results) : formatDiscoveryReport(results);
    std::fwrite(text.data(), 1, text.size(), stdout);
    int rc = 0;
    for (const std::string& id : required) {
        bool ok = false;
        bool known = false;
        for (const PluginResult& r : results) {
            if (r.id == id) {
                known = true;
                ok = r.usable();
            }
        }
        if (!known) {
            std::fprintf(stderr, "fuse_relight_plugins: unknown plugin id '%s'\n", id.c_str());
            return 2;
        }
        if (!ok) {
            rc = 1;
        }
    }
    return rc;
}
