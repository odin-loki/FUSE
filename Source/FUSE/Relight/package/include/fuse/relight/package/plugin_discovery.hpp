// FUSE Relight RL-6.4: optional runtime plugin discovery (docs/plans/FUSE_REMIX_PORT_PLAN.md §0.3, RL-6.4).
//
// FUSE Relight ships no NVIDIA or Intel binary. DLSS SR / RR and Reflex (through Streamline or the NGX
// bridge), Intel XeSS and NRD are optional plugins the user installs. This module answers, before any
// library is loaded, "is plugin X installed, where, and is its version one FUSE supports?", and writes
// one clear log line per plugin. The renderer loaders (Renderer/plugins/{nvidia,nvidia_nrd,intel_xess})
// still do the real load and ABI checks; discovery hands them the directory it found.
//
// Search order per plugin (first directory holding any of its files wins):
//   1. the plugin's own environment variable (FUSE_NVIDIA_SDK_DIR, FUSE_XESS_SDK_DIR, FUSE_NRD_SDK_DIR),
//      the same variables the renderer loaders read;
//   2. $FUSE_RELIGHT_PLUGIN_DIR/<subdir>;
//   3. <directory of the Relight d3d9.dll>/fuse_relight_plugins/<subdir> (the packaged layout).
// Versions come from the PE VS_FIXEDFILEINFO resource (Windows DLLs, parsed portably from the file
// bytes, nothing is loaded) or from the numeric suffix of a Linux soname file (libfoo.so.3.7.10).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::package {

enum class PluginPlatform : std::uint8_t { Windows, Linux };

/// Platform of this build (Windows for MinGW / MSVC, Linux otherwise).
PluginPlatform hostPluginPlatform() noexcept;

enum class PluginStatus : std::uint8_t {
    Available,      ///< every file present, every version in range
    Absent,         ///< no file of the plugin in any searched directory (normal: plugins are optional)
    Incomplete,     ///< some files found, others missing
    VersionTooOld,  ///< a versioned file is older than the minimum
    VersionTooNew,  ///< a versioned file's major is newer than FUSE supports
    VersionUnknown, ///< a versioned file has no readable version; usable, the loader checks the ABI
};

std::string_view toString(PluginStatus status) noexcept;

struct FileVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::uint32_t build = 0;

    friend bool operator==(const FileVersion&, const FileVersion&) = default;
};

/// -1 / 0 / +1 (lexicographic major, minor, patch, build).
int compareVersions(const FileVersion& a, const FileVersion& b) noexcept;
std::string formatVersion(const FileVersion& v);

/// A version constraint on one of the plugin's files. maxMajor 0 means "no upper bound".
struct PluginVersionRule {
    std::string file;
    FileVersion minimum;
    std::uint32_t maxMajor = 0;
};

/// One optional plugin. File names may end in '*' (prefix match, e.g. "libnvidia-ngx-dlss.so.*"); on
/// Windows names match case-insensitively.
struct PluginSpec {
    std::string id;       ///< "dlss", "dlss_rr", "reflex", "xess", "nrd"
    std::string title;    ///< human-readable name for the log
    std::string envVar;   ///< per-plugin directory variable (may be empty)
    std::string subdir;   ///< "<root>/<subdir>" under FUSE_RELIGHT_PLUGIN_DIR and fuse_relight_plugins/
    std::vector<std::string> files;
    std::vector<PluginVersionRule> versions;
};

/// The plugins FUSE Relight knows, with the file names and minimum versions for a platform.
std::vector<PluginSpec> defaultPluginSpecs(PluginPlatform platform);

struct DiscoveryConfig {
    PluginPlatform platform = hostPluginPlatform();
    std::string moduleDir; ///< directory of the Relight d3d9.dll (or the tool); empty: skipped
    /// Environment lookup; empty function: the process environment.
    std::function<std::string(const char* name)> getenv;
};

struct PluginResult {
    std::string id;
    std::string title;
    PluginStatus status = PluginStatus::Absent;
    std::string dir;                    ///< directory the plugin was found in (empty when Absent)
    std::vector<std::string> searched;  ///< directories searched, in order
    std::vector<std::string> missing;   ///< required files not found in dir
    std::vector<std::string> versions;  ///< "<file> <version>" for every versioned file found
    std::string detail;                 ///< one-line human explanation, including what to do

    bool usable() const noexcept { return status == PluginStatus::Available || status == PluginStatus::VersionUnknown; }
};

std::vector<PluginResult> discoverPlugins(const DiscoveryConfig& config, const std::vector<PluginSpec>& specs);
inline std::vector<PluginResult> discoverPlugins(const DiscoveryConfig& config) {
    return discoverPlugins(config, defaultPluginSpecs(config.platform));
}

/// One line per plugin: "fuse-relight plugins: <id>: <status>: <detail>".
std::string formatDiscoveryReport(const std::vector<PluginResult>& results);
/// Machine-readable report (JSON array of {id, status, dir, versions, missing, searched, detail}).
std::string formatDiscoveryJson(const std::vector<PluginResult>& results);

/// Parses the VS_FIXEDFILEINFO file version out of a PE image (PE32 or PE32+) held in memory. Walks the
/// headers, the section table and the resource directory (RT_VERSION); every access is bounds-checked,
/// so arbitrary bytes are safe. False when the image has no version resource.
bool parsePeFileVersion(const std::uint8_t* data, std::size_t size, FileVersion& out) noexcept;
/// Reads a file and parses it (files larger than 256 MiB are rejected).
bool readPeFileVersion(const std::string& path, FileVersion& out);
/// "libnvidia-ngx-dlss.so.310.1.0" -> 310.1.0; false without a numeric suffix after ".so.".
bool parseSonameVersion(std::string_view fileName, FileVersion& out) noexcept;

} // namespace fuse::relight::package
