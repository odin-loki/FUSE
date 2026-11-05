// Qt-free project tools (see editor_project_tools.hpp).
#include <fuse/editor/editor_project_tools.hpp>

#include <fuse/project/loader.hpp>
#include <fuse/project/world_converter.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fuse::editor {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}
std::string extOf(const fs::path& p) {
    std::string e = p.extension().string();
    return lower(e.empty() ? e : e.substr(1));
}
std::string generic(const fs::path& p) { return p.generic_string(); }
std::string tempLogPath() {
    static int counter = 0;
    std::error_code ec;
    return (fs::temp_directory_path(ec) / ("fuse_proc_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                          std::to_string(++counter) + ".log"))
        .string();
}
std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string jsonEscape(std::string_view s) {
    std::string r;
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        if (static_cast<unsigned char>(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
            r += b;
            continue;
        }
        r += c;
    }
    return r;
}
std::string humanBytes(u64 b) {
    char buf[32];
    if (b < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", double(b) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.1f MB", double(b) / (1024.0 * 1024.0));
    return buf;
}
bool isExecutableFile(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
#ifdef _WIN32
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

} // namespace

// ---- processes ------------------------------------------------------------------------------------------------------
#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
// MSVCRT argv quoting (backslashes before a quote are doubled).
void appendArg(std::wstring& cmd, const std::wstring& a) {
    if (!cmd.empty()) cmd += L' ';
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += a;
        return;
    }
    cmd += L'"';
    std::size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') cmd.append(slashes * 2 + 1, L'\\');
        else cmd.append(slashes, L'\\');
        slashes = 0;
        cmd += c;
    }
    cmd.append(slashes * 2, L'\\');
    cmd += L'"';
}
} // namespace

ProcessResult runProcess(const std::string& program, const std::vector<std::string>& args, const std::string& workingDir, u32 timeoutMs) {
    ProcessResult r;
    const std::string log = tempLogPath();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE sink = CreateFileW(widen(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (sink == INVALID_HANDLE_VALUE) return r;
    std::wstring cmd;
    appendArg(cmd, widen(program));
    for (const std::string& a : args) appendArg(cmd, widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = sink;
    si.hStdError = sink;
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = widen(workingDir);
    r.started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                               cwd.empty() ? nullptr : cwd.c_str(), &si, &pi) != FALSE;
    CloseHandle(sink);
    if (r.started) {
        r.finished = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
        if (!r.finished) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = int(code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    r.output = readFile(log);
    std::error_code ec;
    fs::remove(log, ec);
    return r;
}

std::string executableDir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    return fs::path(buf).parent_path().string();
}
#else
ProcessResult runProcess(const std::string& program, const std::vector<std::string>& args, const std::string& workingDir, u32 timeoutMs) {
    ProcessResult r;
    const std::string log = tempLogPath();
    std::vector<std::string> all;
    all.push_back(program);
    all.insert(all.end(), args.begin(), args.end());
    if (!workingDir.empty()) {
        // posix_spawn has no portable chdir action: run through sh -c 'cd dir && exec "$@"'
        all.insert(all.begin(), {"/bin/sh", "-c", "cd \"$0\" && exec \"$@\"", workingDir});
    }
    std::vector<char*> argv;
    for (std::string& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = 0;
    r.started = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    if (r.started) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        int status = 0;
        for (;;) {
            const pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) { r.finished = true; break; }
            if (w < 0 || std::chrono::steady_clock::now() > end) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!r.finished) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    r.output = readFile(log);
    std::error_code ec;
    fs::remove(log, ec);
    return r;
}

std::string executableDir() {
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path().string();
}
#endif

// ---- Project Settings -----------------------------------------------------------------------------------------------
ProjectSettingsLoad loadProjectSettings(const std::string& projectRoot) {
    ProjectSettingsLoad s;
    const project::LoadResult r = project::loadFromDirectory(projectRoot);
    s.loaded = r.status == project::LoadStatus::Ok;
    s.manifest = r.manifest;
    s.error = r.error;
    return s;
}

bool saveProjectSettings(const std::string& projectRoot, project::ProjectManifest m, std::string* error) {
    auto trim = [](std::string& t) {
        const auto b = t.find_first_not_of(" \t\r\n");
        const auto e = t.find_last_not_of(" \t\r\n");
        t = b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
    };
    trim(m.name);
    trim(m.defaultWorld3D);
    trim(m.defaultWorld2D);
    if (m.name.empty()) {
        if (error) *error = "Project name must not be empty";
        return false;
    }
    if (!m.dimensions.enable3D && !m.dimensions.enable2D) {
        if (error) *error = "Enable at least one of 3D / 2D";
        return false;
    }
    m.schemaVersion = project::kProjectSchemaVersion;
    const project::SaveResult r = project::saveToDirectory(m, projectRoot);
    if (!r.ok && error) *error = "Cannot write project.json: " + r.error;
    return r.ok;
}

// ---- Import Asset ---------------------------------------------------------------------------------------------------
std::string assetImportKind(std::string_view extension) {
    const std::string s = lower(extension);
    auto any = [&](std::initializer_list<const char*> l) { return std::any_of(l.begin(), l.end(), [&](const char* x) { return s == x; }); };
    if (any({"png", "jpg", "jpeg", "tga", "bmp", "hdr", "exr", "dds", "ktx", "ktx2"})) return "textures";
    if (any({"gltf", "glb", "obj", "fbx"})) return "models";
    if (any({"wav", "ogg", "mp3", "flac"})) return "audio";
    if (any({"fuselevel", "fusescene", "prefab"})) return "scenes";
    if (s == "lua") return "scripts";
    return "misc";
}

std::vector<std::string> importAssetsIntoProject(const std::string& projectRoot, const std::vector<std::string>& sources, std::string* error) {
    std::vector<std::string> written;
    const fs::path root(projectRoot);
    for (const std::string& src : sources) {
        const fs::path in(src);
        std::error_code ec;
        if (!fs::is_regular_file(in, ec)) {
            if (error) *error = "not a file: " + src;
            continue;
        }
        const std::string rel = "assets/" + assetImportKind(extOf(in));
        fs::create_directories(root / rel, ec);
        std::string name = in.filename().string();
        for (int n = 1; fs::exists(root / rel / name, ec); ++n)
            name = in.stem().string() + "_" + std::to_string(n) + in.extension().string();
        const std::string dst = rel + "/" + name;
        if (fs::copy_file(in, root / dst, ec)) written.push_back(dst);
        else if (error) *error = "copy failed: " + src;
    }
    return written;
}

std::string findCookTool() {
    if (const char* env = std::getenv("FUSE_COOK"); env && *env && isExecutableFile(env)) return env;
#ifdef _WIN32
    const std::string exe = "fuse_cook.exe";
    const char sep = ';';
#else
    const std::string exe = "fuse_cook";
    const char sep = ':';
#endif
    const fs::path app(executableDir());
    for (const std::string rel : {exe, "../../../Tools/FUSE/" + exe, "../../../../Tools/FUSE/" + exe, "../../../../../Tools/FUSE/" + exe,
                                  "../Tools/FUSE/" + exe}) {
        const fs::path p = app / rel;
        std::error_code ec;
        if (isExecutableFile(p)) return fs::weakly_canonical(p, ec).string();
    }
    if (const char* path = std::getenv("PATH")) {
        std::string_view rest(path);
        while (!rest.empty()) {
            const auto cut = rest.find(sep);
            const fs::path p = fs::path(std::string(rest.substr(0, cut))) / exe;
            if (isExecutableFile(p)) return p.string();
            if (cut == std::string_view::npos) break;
            rest.remove_prefix(cut + 1);
        }
    }
    return {};
}

bool makeAssetCookJob(const std::string& projectRoot, const std::string& relPath, const std::string& cookTool, AssetCookJob& job) {
    if (cookTool.empty()) return false;
    const fs::path in(relPath);
    const std::string ext = extOf(in), kind = assetImportKind(ext);
    std::string mode, outExt;
    if (kind == "models") { mode = "--mesh"; outExt = "fusemesh"; }
    else if (kind == "textures" && ext != "dds" && ext != "exr" && ext != "bmp") { mode = "--texture"; outExt = "fusetex"; }
    else if (kind == "audio" && ext != "mp3") { mode = "--audio"; outExt = "fuseaudio"; }
    else return false;
    const fs::path root(projectRoot);
    const std::string outRel = "assets/cooked/" + kind + "/" + in.stem().string() + "." + outExt;
    std::error_code ec;
    fs::create_directories((root / outRel).parent_path(), ec);
    job.program = cookTool;
    job.arguments = {mode, "--input", (root / relPath).string(), "--output", (root / outRel).string()};
    if (kind == "textures" && ext == "hdr") job.arguments.push_back("--hdr");
    job.source = relPath;
    job.output = outRel;
    return true;
}

// ---- Build / Package ------------------------------------------------------------------------------------------------
bool wildcardMatch(std::string_view p, std::string_view t) {
    // iterative glob with backtracking on the last '*'
    std::size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
    auto eq = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || eq(p[pi], t[ti]))) { ++pi; ++ti; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; }
        else if (star != std::string_view::npos) { pi = star + 1; ti = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::vector<std::string> defaultPackageExcludes() { return {"*.psd", "*.blend", "*.blend1", "*.kra", "*.xcf", "Thumbs.db"}; }

namespace {
bool skipDir(const std::string& rel) {
    std::stringstream ss(rel);
    for (std::string part; std::getline(ss, part, '/');)
        if ((!part.empty() && part[0] == '.') || part == "build") return true;
    return rel.rfind("assets/cooked", 0) == 0;
}
std::string packageName(const std::string& root) {
    const ProjectSettingsLoad s = loadProjectSettings(root);
    if (!s.manifest.name.empty()) {
        std::string safe;
        for (char c : s.manifest.name) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
        return safe;
    }
    return fs::path(root).filename().string();
}
bool matchesAny(const std::string& rel, const std::vector<std::string>& patterns) {
    const std::string name = fs::path(rel).filename().string();
    for (std::string pat : patterns) {
        const auto b = pat.find_first_not_of(' ');
        const auto e = pat.find_last_not_of(' ');
        if (b == std::string::npos) continue;
        pat = pat.substr(b, e - b + 1);
        if (wildcardMatch(pat, rel) || wildcardMatch(pat, name)) return true;
    }
    return false;
}
const std::vector<std::string>& shippingExcludes() {
    static const std::vector<std::string> p = {"*.prefablinks", "*.variant", "*.variantbase", "*.bak", "editor_layout*.ini", "*.fuselevel.names"};
    return p;
}
bool writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return bool(f);
}
} // namespace

PackageResult packageProject(const PackageOptions& opts, const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    PackageResult r;
    std::error_code ec;
    if (opts.projectRoot.empty() || !fs::exists(fs::path(opts.projectRoot) / "project.json", ec)) {
        r.error = "No project.json in " + opts.projectRoot;
        return r;
    }
    if (opts.outputDir.empty()) {
        r.error = "No output folder";
        return r;
    }
    const fs::path src = fs::weakly_canonical(opts.projectRoot, ec);
    const fs::path pkg = fs::weakly_canonical(fs::absolute(opts.outputDir, ec) / packageName(opts.projectRoot), ec);
    {
        const std::string s = generic(src) + "/", p = generic(pkg) + "/";
        if (p.rfind(s, 0) == 0) {
            r.error = "Output folder must be outside the project";
            return r;
        }
    }
    r.packageDir = pkg.string();
    if (opts.clean && fs::exists(pkg, ec)) {
        say("Cleaning " + r.packageDir);
        fs::remove_all(pkg, ec);
    }
    const fs::path data = pkg / "project";
    fs::create_directories(data, ec);

    // 1. copy
    say("Copying project files...");
    std::vector<std::string> excludes = opts.excludePatterns;
    const bool shipping = lower(opts.configuration) == "shipping";
    if (shipping) excludes.insert(excludes.end(), shippingExcludes().begin(), shippingExcludes().end());
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path& abs : files) {
        const std::string rel = generic(fs::relative(abs, src, ec));
        const std::string dir = generic(fs::path(rel).parent_path());
        if (skipDir(dir) || fs::path(rel).filename().string().rfind('.', 0) == 0) continue;
        if (matchesAny(rel, excludes)) {
            ++r.excludedFiles;
            continue;
        }
        const fs::path dst = data / rel;
        fs::create_directories(dst.parent_path(), ec);
        if (!fs::copy_file(abs, dst, fs::copy_options::overwrite_existing, ec)) {
            r.error = "Copy failed: " + rel;
            return r;
        }
        ++r.copiedFiles;
        r.totalBytes += fs::file_size(abs, ec);
    }
    say("Copied " + std::to_string(r.copiedFiles) + " file(s), " + humanBytes(r.totalBytes) + " (" + (shipping ? "Shipping" : "Development") +
        " configuration" + (r.excludedFiles > 0 ? ", " + std::to_string(r.excludedFiles) + " excluded" : std::string()) + ")");

    // the default 3D world: a project that ships only the legacy .mis (Samples) gets its .fuselevel in the package
    // (converted from the staged copy, as the editor and runtime do on open)
    if (const project::LoadResult staged = project::loadFromDirectory(data.string());
        staged.status == project::LoadStatus::Ok && !staged.manifest.defaultWorld3D.empty()) {
        const fs::path level = data / staged.manifest.defaultWorld3D;
        const fs::path mission = fs::path(level).replace_extension(".mis");
        if (!fs::exists(level, ec) && fs::exists(mission, ec) &&
            project::convertT3DMissionToFuselevel(mission.string(), level.string()).status == project::ConvertStatus::Ok)
            say("Converted " + generic(fs::relative(mission, data, ec)) + " to " + staged.manifest.defaultWorld3D);
    }

    // 2. cook (project-wide fuse_cook over the staged copy)
    if (opts.cookAssets) {
        const std::string tool = findCookTool();
        if (tool.empty()) {
            say("fuse_cook not found; skipping cook (set FUSE_COOK)");
        } else {
            const ProcessResult p = runProcess(tool, {"--project", data.string()});
            std::string prefix = generic(fs::absolute(data, ec)) + "/";
            std::stringstream ss(p.output);
            for (std::string line; std::getline(ss, line);) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                const auto b = line.find_first_not_of(' ');
                if (b == std::string::npos || line[b] != '[') continue;
                line = line.substr(b);
                std::replace(line.begin(), line.end(), '\\', '/');
                for (std::size_t at; (at = line.find(prefix)) != std::string::npos;) line.erase(at, prefix.size());
                const auto arrow = line.find(" -> ");
                const auto paren = arrow == std::string::npos ? std::string::npos : line.find(" (", arrow);
                if (paren == std::string::npos) continue;
                const std::string out = line.substr(arrow + 4, paren - arrow - 4);
                if (line.find("(ok)") != std::string::npos) {
                    ++r.cookedAssets;
                    r.cooked.push_back(out);
                    say("Cooked " + line.substr(0, paren));
                } else {
                    ++r.failedCooks;
                    say("Cook FAILED " + line);
                }
            }
            if (!p.finished || (p.exitCode != 0 && r.failedCooks == 0)) {
                ++r.failedCooks;
                say("fuse_cook --project failed (exit " + std::to_string(p.exitCode) + ")");
            }
            fs::remove_all(data / ".fuse", ec); // the cook cache is not shipped
        }
    }

    // 3. runtime + launchers
    std::string runtimeName;
    if (!opts.runtimePath.empty()) {
        const fs::path rt(opts.runtimePath);
        if (!fs::is_regular_file(rt, ec)) {
            r.error = "Runtime not found: " + opts.runtimePath;
            return r;
        }
        runtimeName = rt.filename().string();
        fs::create_directories(pkg / "bin", ec);
        if (!fs::copy_file(rt, pkg / "bin" / runtimeName, fs::copy_options::overwrite_existing, ec)) {
            r.error = "Cannot copy runtime";
            return r;
        }
        const auto exec = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
        fs::permissions(pkg / "bin" / runtimeName, exec, fs::perm_options::add, ec);
        std::string base = runtimeName;
        if (base.size() > 4 && lower(base.substr(base.size() - 4)) == ".exe") base.resize(base.size() - 4);
        writeText(pkg / "run.sh", "#!/bin/sh\ncd \"$(dirname \"$0\")\" || exit 1\nexport LD_LIBRARY_PATH=\"$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n"
                                  "exec ./bin/" + base + " --present project \"$@\"\n");
        fs::permissions(pkg / "run.sh", exec, fs::perm_options::add, ec);
        writeText(pkg / "run.bat", "@echo off\r\ncd /d \"%~dp0\"\r\nbin\\" + base + ".exe --present project %*\r\n");
        say("Runtime: bin/" + runtimeName + " (+ run.sh / run.bat)");
    }

    // 4. manifest
    {
        const std::time_t now = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        std::string cooked;
        for (const std::string& c : r.cooked) cooked += (cooked.empty() ? "\"" : ", \"") + jsonEscape(c) + "\"";
        std::string j = "{\n";
        j += "    \"project\": \"" + jsonEscape(packageName(opts.projectRoot)) + "\",\n";
        j += "    \"source\": \"" + jsonEscape(generic(src)) + "\",\n";
        j += "    \"created\": \"" + std::string(when) + "\",\n";
        j += "    \"files\": " + std::to_string(r.copiedFiles) + ",\n";
        j += "    \"cooked\": [" + cooked + "],\n";
        j += "    \"failedCooks\": " + std::to_string(r.failedCooks) + ",\n";
        j += "    \"configuration\": \"" + std::string(shipping ? "Shipping" : "Development") + "\",\n";
        j += "    \"excluded\": " + std::to_string(r.excludedFiles) + ",\n";
        j += "    \"bytes\": " + std::to_string(r.totalBytes) + ",\n";
        j += "    \"runtime\": \"" + (runtimeName.empty() ? std::string() : "bin/" + jsonEscape(runtimeName)) + "\"\n}\n";
        if (!writeText(pkg / "package.json", j)) {
            r.error = "Cannot write package.json";
            return r;
        }
    }
    r.ok = r.failedCooks == 0;
    if (r.ok && opts.archive) {
        const fs::path archive = fs::absolute(opts.outputDir, ec) / (pkg.filename().string() + ".tar.gz");
        fs::remove(archive, ec);
        const ProcessResult t = runProcess("tar", {"-czf", archive.string(), pkg.filename().string()}, fs::absolute(opts.outputDir, ec).string());
        if (t.finished && t.exitCode == 0 && fs::exists(archive, ec)) {
            r.archivePath = archive.string();
            say("Archive: " + r.archivePath + " (" + humanBytes(fs::file_size(archive, ec)) + ")");
        } else {
            r.ok = false;
            r.error = "Archive failed: " + t.output;
            return r;
        }
    }
    if (!r.ok) r.error = std::to_string(r.failedCooks) + " asset(s) failed to cook";
    say(r.ok ? "Package ready: " + r.packageDir : r.error);
    return r;
}

} // namespace fuse::editor
