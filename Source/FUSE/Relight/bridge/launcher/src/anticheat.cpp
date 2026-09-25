// FUSE Relight RL-2.3: launcher checks that do not need Win32 — the anti-cheat marker table and
// scans, PE machine detection, command-line quoting (see launcher.hpp).
// Copyright (c) 2026 FUSE contributors (AGPL-3.0). New code.

#include <fuse/relight/bridge/launcher/launcher.hpp>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fuse::relight::bridge::launcher {

namespace fs = std::filesystem;

const std::vector<AntiCheatMarker>& knownAntiCheatMarkers() {
    static const std::vector<AntiCheatMarker> markers = {
#define RL_ANTICHEAT(product, kind, pattern, note) {#product, MarkerKind::kind, pattern, note},
#include "../anticheat_list.inc"
#undef RL_ANTICHEAT
    };
    return markers;
}

const char* toString(MarkerKind k) noexcept {
    switch (k) {
    case MarkerKind::Process: return "running process";
    case MarkerKind::Directory: return "directory";
    case MarkerKind::File: return "file";
    case MarkerKind::ExeName: return "executable name";
    }
    return "?";
}

const char* toString(LaunchStatus s) noexcept {
    switch (s) {
    case LaunchStatus::Ok: return "ok";
    case LaunchStatus::RefusedAntiCheat: return "refused: anti-cheat detected";
    case LaunchStatus::ArchMismatch: return "refused: executable architecture differs from the launcher";
    case LaunchStatus::NotFound: return "executable not found";
    case LaunchStatus::CreateFailed: return "CreateProcess failed";
    case LaunchStatus::InjectFailed: return "DLL injection failed";
    case LaunchStatus::Unsupported: return "not supported on this platform";
    }
    return "?";
}

int exitCodeFor(LaunchStatus s) noexcept {
    switch (s) {
    case LaunchStatus::Ok: return 0;
    case LaunchStatus::RefusedAntiCheat: return kExitRefusedAntiCheat;
    case LaunchStatus::ArchMismatch: return kExitArchMismatch;
    case LaunchStatus::NotFound: return kExitNotFound;
    case LaunchStatus::CreateFailed: return kExitCreateFailed;
    case LaunchStatus::InjectFailed: return kExitInjectFailed;
    case LaunchStatus::Unsupported: return kExitUsage;
    }
    return kExitUsage;
}

bool globMatch(std::string_view pattern, std::string_view name) noexcept {
    // Iterative '*' matching with one backtrack point (sufficient for a single-wildcard class).
    size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
    auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (p < pattern.size() && eq(pattern[p], name[n])) {
            ++p;
            ++n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

namespace {

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Case-insensitive lookup of a '/'-separated relative path under dir; returns the matched path.
bool findRelative(const fs::path& dir, std::string_view rel, bool wantDirectory, std::string& found) {
    fs::path cur = dir;
    size_t start = 0;
    for (;;) {
        const size_t slash = rel.find('/', start);
        const std::string_view part = rel.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        const bool last = slash == std::string_view::npos;
        std::error_code ec;
        fs::directory_iterator it(cur, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            return false;
        }
        bool matched = false;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                return false;
            }
            const std::string name = utf8String(it->path().filename());
            if (!globMatch(part, name)) {
                continue;
            }
            std::error_code sec;
            const bool isDir = it->is_directory(sec);
            if (last ? (isDir == wantDirectory) : isDir) {
                cur = it->path();
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
        if (last) {
            found = utf8String(cur);
            return true;
        }
        start = slash + 1;
    }
}

}  // namespace

std::vector<AntiCheatHit> scanGameInstall(const std::string& exePath) {
    std::vector<AntiCheatHit> hits;
    const std::string exeName = baseName(exePath);
    std::error_code ec;
    fs::path exe = fs::absolute(utf8Path(exePath), ec);
    if (ec) {
        exe = utf8Path(exePath);
    }
    std::vector<fs::path> dirs;
    const fs::path dir = exe.parent_path();
    if (!dir.empty()) {
        dirs.push_back(dir);
        if (dir.has_parent_path() && dir.parent_path() != dir) {
            dirs.push_back(dir.parent_path());
        }
    }
    for (const AntiCheatMarker& m : knownAntiCheatMarkers()) {
        if (m.kind == MarkerKind::ExeName) {
            if (globMatch(m.pattern, exeName)) {
                hits.push_back({&m, exePath});
            }
            continue;
        }
        if (m.kind != MarkerKind::Directory && m.kind != MarkerKind::File) {
            continue;
        }
        for (const fs::path& d : dirs) {
            std::string found;
            if (findRelative(d, m.pattern, m.kind == MarkerKind::Directory, found)) {
                hits.push_back({&m, found});
                break;
            }
        }
    }
    return hits;
}

std::vector<AntiCheatHit> scanProcesses(const std::vector<std::string>& imageNames) {
    std::vector<AntiCheatHit> hits;
    for (const AntiCheatMarker& m : knownAntiCheatMarkers()) {
        if (m.kind != MarkerKind::Process) {
            continue;
        }
        for (const std::string& n : imageNames) {
            if (globMatch(m.pattern, baseName(n))) {
                hits.push_back({&m, n});
                break;
            }
        }
    }
    return hits;
}

bool readPeMachine(const std::string& path, uint16_t& machine) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) {
        return false;
    }
    char mz[64] = {};
    if (!f.read(mz, sizeof(mz)) || mz[0] != 'M' || mz[1] != 'Z') {
        return false;
    }
    uint32_t peOffset = 0;
    std::memcpy(&peOffset, mz + 0x3c, 4);
    if (peOffset > (1u << 20)) {
        return false;
    }
    f.seekg(peOffset);
    char pe[6] = {};
    if (!f.read(pe, sizeof(pe)) || std::memcmp(pe, "PE\0\0", 4) != 0) {
        return false;
    }
    std::memcpy(&machine, pe + 4, 2);
    return true;
}

std::string quoteArgument(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

#if !defined(_WIN32)
std::vector<std::string> runningProcessNames() {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::ifstream comm(it->path() / "comm");
        std::string name;
        if (comm && std::getline(comm, name) && !name.empty()) {
            names.push_back(name);
        }
    }
    return names;
}

LaunchReport launch(const LaunchSpec& spec, const std::vector<std::string>* processNames) {
    LaunchReport rep;
    rep.antiCheat = scanGameInstall(spec.exe);
    const std::vector<AntiCheatHit> procs = scanProcesses(processNames ? *processNames : runningProcessNames());
    rep.antiCheat.insert(rep.antiCheat.end(), procs.begin(), procs.end());
    rep.status = rep.antiCheat.empty() ? LaunchStatus::Unsupported : LaunchStatus::RefusedAntiCheat;
    rep.message = toString(rep.status);
    return rep;
}

int runSelfTest(bool) { return 77; }
#endif

}  // namespace fuse::relight::bridge::launcher
