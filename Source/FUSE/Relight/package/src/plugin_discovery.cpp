// FUSE Relight RL-6.4: optional runtime plugin discovery (see plugin_discovery.hpp). FUSE's own code (MIT).
#include <fuse/relight/package/plugin_discovery.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fuse::relight::package {

namespace fs = std::filesystem;

PluginPlatform hostPluginPlatform() noexcept {
#if defined(_WIN32)
    return PluginPlatform::Windows;
#else
    return PluginPlatform::Linux;
#endif
}

std::string_view toString(PluginStatus status) noexcept {
    switch (status) {
        case PluginStatus::Available: return "available";
        case PluginStatus::Absent: return "absent";
        case PluginStatus::Incomplete: return "incomplete";
        case PluginStatus::VersionTooOld: return "version too old";
        case PluginStatus::VersionTooNew: return "version too new";
        case PluginStatus::VersionUnknown: return "version unknown";
    }
    return "unknown";
}

int compareVersions(const FileVersion& a, const FileVersion& b) noexcept {
    const std::uint32_t x[4] = {a.major, a.minor, a.patch, a.build};
    const std::uint32_t y[4] = {b.major, b.minor, b.patch, b.build};
    for (int i = 0; i < 4; ++i) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

std::string formatVersion(const FileVersion& v) {
    std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    if (v.build != 0) {
        s += "." + std::to_string(v.build);
    }
    return s;
}

std::vector<PluginSpec> defaultPluginSpecs(PluginPlatform platform) {
    std::vector<PluginSpec> specs;
    const std::string nv = "FUSE_NVIDIA_SDK_DIR";
    if (platform == PluginPlatform::Windows) {
        // Streamline 2.x provider (Renderer/plugins/nvidia, fuse_nvplugin_streamline.dll) + NVIDIA's signed DLLs.
        specs.push_back({"dlss", "NVIDIA DLSS Super Resolution (Streamline)", nv, "nvidia",
                         {"fuse_nvplugin_streamline.dll", "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "nvngx_dlss.dll"},
                         {{"sl.interposer.dll", {2, 0, 0, 0}, 2}, {"nvngx_dlss.dll", {3, 1, 0, 0}, 0}}});
        specs.push_back({"dlss_rr", "NVIDIA DLSS Ray Reconstruction (Streamline)", nv, "nvidia",
                         {"fuse_nvplugin_streamline.dll", "sl.interposer.dll", "sl.common.dll", "sl.dlss_d.dll", "nvngx_dlssd.dll"},
                         {{"sl.interposer.dll", {2, 0, 0, 0}, 2}, {"nvngx_dlssd.dll", {3, 5, 0, 0}, 0}}});
        specs.push_back({"reflex", "NVIDIA Reflex (Streamline)", nv, "nvidia",
                         {"fuse_nvplugin_streamline.dll", "sl.interposer.dll", "sl.common.dll", "sl.reflex.dll", "sl.pcl.dll"},
                         {{"sl.interposer.dll", {2, 0, 0, 0}, 2}, {"sl.reflex.dll", {2, 0, 0, 0}, 2}}});
        // Same supported range as Renderer/plugins/intel_xess (XessRuntime::kMinMajor..kMaxMajor).
        specs.push_back({"xess", "Intel XeSS", "FUSE_XESS_SDK_DIR", "xess", {"libxess.dll"}, {{"libxess.dll", {1, 0, 0, 0}, 2}}});
        specs.push_back({"nrd", "NVIDIA NRD denoiser", "FUSE_NRD_SDK_DIR", "nrd",
                         {"fuse_nrdplugin_nri.dll", "NRD.dll"}, {{"NRD.dll", {4, 0, 0, 0}, 4}}});
    } else {
        // NGX bridge (libfuse_nvplugin_ngx.so) next to the DLSS SDK's versioned runtime.
        specs.push_back({"dlss", "NVIDIA DLSS Super Resolution (NGX)", nv, "nvidia",
                         {"libfuse_nvplugin_ngx.so", "libnvidia-ngx-dlss.so.*"},
                         {{"libnvidia-ngx-dlss.so.*", {3, 1, 0, 0}, 0}}});
        specs.push_back({"dlss_rr", "NVIDIA DLSS Ray Reconstruction (NGX)", nv, "nvidia",
                         {"libfuse_nvplugin_ngx.so", "libnvidia-ngx-dlssd.so.*"},
                         {{"libnvidia-ngx-dlssd.so.*", {3, 5, 0, 0}, 0}}});
        specs.push_back({"xess", "Intel XeSS", "FUSE_XESS_SDK_DIR", "xess", {"libxess.so"}, {}});
        specs.push_back({"nrd", "NVIDIA NRD denoiser", "FUSE_NRD_SDK_DIR", "nrd", {"libfuse_nrdplugin_nri.so", "libNRD.so"}, {}});
    }
    return specs;
}

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

bool nameMatches(const std::string& candidate, const std::string& pattern, bool caseInsensitive) {
    const std::string c = caseInsensitive ? lower(candidate) : candidate;
    const std::string p = caseInsensitive ? lower(pattern) : pattern;
    if (!p.empty() && p.back() == '*') {
        return c.size() >= p.size() - 1 && c.compare(0, p.size() - 1, p, 0, p.size() - 1) == 0;
    }
    return c == p;
}

// Regular files of a directory (names only); empty when the directory does not exist.
std::vector<std::string> listFiles(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(fs::path(dir), ec)) {
        return names;
    }
    for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec)) {
            names.push_back(it->path().filename().string());
        }
    }
    std::sort(names.begin(), names.end()); // deterministic pick among prefix matches
    return names;
}

// The matching file name; for a prefix pattern the highest version wins.
std::string findFile(const std::vector<std::string>& names, const std::string& pattern, PluginPlatform platform) {
    const bool ci = platform == PluginPlatform::Windows;
    std::string best;
    FileVersion bestV;
    for (const std::string& n : names) {
        if (!nameMatches(n, pattern, ci)) {
            continue;
        }
        FileVersion v;
        const bool hasV = parseSonameVersion(n, v);
        if (best.empty() || (hasV && compareVersions(v, bestV) > 0)) {
            best = n;
            bestV = hasV ? v : FileVersion{};
        }
    }
    return best;
}

std::string joinDir(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    return (fs::path(a) / b).string(); // native separator (backslash on Windows)
}

std::string envValue(const DiscoveryConfig& config, const char* name) {
    if (config.getenv) {
        return config.getenv(name);
    }
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string joinList(const std::vector<std::string>& items) {
    std::string s;
    for (const std::string& i : items) {
        s += (s.empty() ? "" : ", ") + i;
    }
    return s;
}

} // namespace

std::vector<PluginResult> discoverPlugins(const DiscoveryConfig& config, const std::vector<PluginSpec>& specs) {
    std::vector<PluginResult> results;
    const std::string pluginRoot = envValue(config, "FUSE_RELIGHT_PLUGIN_DIR");
    for (const PluginSpec& spec : specs) {
        PluginResult res;
        res.id = spec.id;
        res.title = spec.title;
        const std::string envDir = spec.envVar.empty() ? std::string() : envValue(config, spec.envVar.c_str());
        if (!envDir.empty()) {
            res.searched.push_back(envDir);
        }
        if (!pluginRoot.empty()) {
            res.searched.push_back(joinDir(pluginRoot, spec.subdir));
        }
        if (!config.moduleDir.empty()) {
            res.searched.push_back(joinDir(joinDir(config.moduleDir, "fuse_relight_plugins"), spec.subdir));
        }

        std::vector<std::string> found; // parallel to spec.files ("" = missing)
        for (const std::string& dir : res.searched) {
            const std::vector<std::string> names = listFiles(dir);
            std::vector<std::string> hits;
            bool any = false;
            for (const std::string& f : spec.files) {
                hits.push_back(findFile(names, f, config.platform));
                any = any || !hits.back().empty();
            }
            if (any) {
                res.dir = dir;
                found = std::move(hits);
                break;
            }
        }

        const std::string where = res.searched.empty() ? std::string("no directory configured") : joinList(res.searched);
        std::string hint = "optional; install the runtime in " +
                           joinDir(joinDir(config.moduleDir.empty() ? std::string("<Relight dir>") : config.moduleDir,
                                           "fuse_relight_plugins"),
                                   spec.subdir);
        if (!spec.envVar.empty()) {
            hint += " or set " + spec.envVar;
        }
        if (res.dir.empty()) {
            res.status = PluginStatus::Absent;
            res.detail = "not installed (searched: " + where + "); " + hint;
            results.push_back(std::move(res));
            continue;
        }
        for (std::size_t i = 0; i < spec.files.size(); ++i) {
            if (found[i].empty()) {
                res.missing.push_back(spec.files[i]);
            }
        }
        if (!res.missing.empty()) {
            res.status = PluginStatus::Incomplete;
            res.detail = "found in " + res.dir + " but missing " + joinList(res.missing) + "; the plugin stays off";
            results.push_back(std::move(res));
            continue;
        }

        res.status = PluginStatus::Available;
        std::string versionProblem;
        std::string unknown;
        for (const PluginVersionRule& rule : spec.versions) {
            std::string file;
            for (std::size_t i = 0; i < spec.files.size(); ++i) {
                if (spec.files[i] == rule.file) {
                    file = found[i];
                }
            }
            if (file.empty()) {
                continue; // the rule names a file outside spec.files: nothing to check
            }
            FileVersion v;
            const std::string path = joinDir(res.dir, file);
            const bool ok = config.platform == PluginPlatform::Windows ? readPeFileVersion(path, v) : parseSonameVersion(file, v);
            if (!ok) {
                unknown += (unknown.empty() ? "" : ", ") + file;
                res.versions.push_back(file + " ?");
                continue;
            }
            res.versions.push_back(file + " " + formatVersion(v));
            if (res.status != PluginStatus::Available) {
                continue; // report the first version problem only
            }
            if (compareVersions(v, rule.minimum) < 0) {
                res.status = PluginStatus::VersionTooOld;
                versionProblem = file + " " + formatVersion(v) + " is older than the minimum " + formatVersion(rule.minimum);
            } else if (rule.maxMajor != 0 && v.major > rule.maxMajor) {
                res.status = PluginStatus::VersionTooNew;
                versionProblem = file + " " + formatVersion(v) + " is newer than supported (major <= " +
                                 std::to_string(rule.maxMajor) + ")";
            }
        }
        if (res.status == PluginStatus::VersionTooOld || res.status == PluginStatus::VersionTooNew) {
            res.detail = versionProblem + " (in " + res.dir + "); the plugin stays off, install a supported version";
        } else if (!unknown.empty()) {
            res.status = PluginStatus::VersionUnknown;
            res.detail = res.dir + ": no version information in " + unknown + "; the loader checks the plugin ABI";
        } else {
            res.detail = res.dir + (res.versions.empty() ? std::string() : " (" + joinList(res.versions) + ")");
        }
        results.push_back(std::move(res));
    }
    return results;
}

std::string formatDiscoveryReport(const std::vector<PluginResult>& results) {
    std::string out;
    for (const PluginResult& r : results) {
        out += "fuse-relight plugins: " + r.id + ": " + std::string(toString(r.status)) + ": " + r.detail + "\n";
    }
    return out;
}

namespace {

std::string jsonString(std::string_view s) {
    std::string o = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    o += "\\u00";
                    o += hex[(c >> 4) & 0xF];
                    o += hex[c & 0xF];
                } else {
                    o += c;
                }
        }
    }
    return o + "\"";
}

std::string jsonArray(const std::vector<std::string>& items) {
    std::string o = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        o += (i ? "," : "") + jsonString(items[i]);
    }
    return o + "]";
}

} // namespace

std::string formatDiscoveryJson(const std::vector<PluginResult>& results) {
    std::string o = "[";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const PluginResult& r = results[i];
        o += std::string(i ? ",\n " : "\n ") + "{\"id\":" + jsonString(r.id) + ",\"status\":" + jsonString(toString(r.status)) +
             ",\"usable\":" + (r.usable() ? "true" : "false") + ",\"dir\":" + jsonString(r.dir) +
             ",\"versions\":" + jsonArray(r.versions) + ",\"missing\":" + jsonArray(r.missing) +
             ",\"searched\":" + jsonArray(r.searched) + ",\"detail\":" + jsonString(r.detail) + "}";
    }
    return o + "\n]\n";
}

} // namespace fuse::relight::package
