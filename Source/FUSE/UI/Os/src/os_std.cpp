// fuse::ui::os on the C++ standard library plus POSIX / Win32 (no third-party OS layer).
// This is the default backend of fuse_ui_os on main. The FORGE-backed one (os_forge.cpp, from the gui-framework
// branch) is built only with -DFUSE_UI_OS_BACKEND=forge -DFUSE_FORGE_DIR=<checkout>.
//
// Scope: everything fuse_ui_extras uses (environment, files, clocks, dynamic libraries, helper processes) is
// implemented for POSIX desktops. Stated limits, each reported through the function's error / empty result:
//   - runProcess: POSIX desktops only (fork + exec). Windows, iOS and Android return an error.
//   - clipboard: only through the wl-clipboard / xclip helpers (no window-less clipboard otherwise).
//   - createFileWatcher: polls modification times (no inotify / kqueue / ReadDirectoryChangesW).
//   - displayHdr: always unknown.
//   - fd plumbing (SharedBuffer, pipes): POSIX only, as in the FORGE backend.
#include "fuse/ui/os.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#endif

// Helper processes exist on POSIX desktops only.
#if !defined(_WIN32) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#define FUSE_UI_OS_HAS_PROCESS 1
#else
#define FUSE_UI_OS_HAS_PROCESS 0
#endif

namespace fuse::ui::os {
namespace fs = std::filesystem;

namespace {
fs::path toPath(std::string_view p) { return fs::path(std::u8string(p.begin(), p.end())); }
std::string fromPath(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}
} // namespace

std::string env(std::string_view name, std::string_view fallback) {
    const std::string n(name);
    const char* v = std::getenv(n.c_str());
    return v && *v ? std::string(v) : std::string(fallback);
}

std::string_view platformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return "ios";
#else
    return "macos";
#endif
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string configDir(std::string_view app) {
    const std::string a(app);
#if defined(_WIN32)
    return env("APPDATA", ".") + "\\" + a;
#elif defined(__APPLE__)
    return env("HOME", ".") + "/Library/Application Support/" + a;
#else
    if (std::string x = env("XDG_CONFIG_HOME"); !x.empty()) return x + "/" + a;
    return env("HOME", ".") + "/.config/" + a;
#endif
}

std::string runtimeDir() {
    if (std::string r = env("XDG_RUNTIME_DIR"); !r.empty()) return r;
    std::error_code ec;
    const fs::path t = fs::temp_directory_path(ec);
    return ec ? std::string(".") : fromPath(t);
}

std::expected<std::string, std::string> readFile(std::string_view path) {
    std::ifstream f(toPath(path), std::ios::binary);
    if (!f) return std::unexpected("cannot read " + std::string(path));
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) return std::unexpected("read error on " + std::string(path));
    return data;
}

std::expected<void, std::string> writeFileAtomic(std::string_view path, std::string_view data) {
    const fs::path target = toPath(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) return std::unexpected("cannot create " + fromPath(target.parent_path()) + ": " + ec.message());
    }
    fs::path tmp = target;
    tmp += ".tmp" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return std::unexpected("cannot write " + fromPath(tmp));
        f.write(data.data(), std::streamsize(data.size()));
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return std::unexpected("write error on " + fromPath(tmp));
        }
    }
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    ec.clear();
    fs::rename(tmp, target, ec);
    if (ec) {
        const std::string msg = "cannot replace " + std::string(path) + ": " + ec.message();
        fs::remove(tmp, ec);
        return std::unexpected(msg);
    }
    return {};
}

bool isDirectory(std::string_view path) {
    std::error_code ec;
    return fs::is_directory(toPath(path), ec);
}
bool exists(std::string_view path) {
    std::error_code ec;
    return fs::exists(toPath(path), ec);
}
std::string absolutePath(std::string_view path) {
    std::error_code ec;
    fs::path a = fs::absolute(toPath(path), ec);
    if (ec) a = toPath(path);
    std::string s = fromPath(a.lexically_normal());
    if (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
std::expected<std::vector<DirEntry>, std::string> listDirectory(std::string_view path) {
    std::error_code ec;
    fs::directory_iterator it(toPath(path), ec);
    if (ec) return std::unexpected("cannot list " + std::string(path) + ": " + ec.message());
    std::vector<DirEntry> out;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        out.push_back({fromPath(it->path().filename()), it->is_directory(e2)});
    }
    return out;
}

double monotonicSeconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
void sleepSeconds(double seconds) {
    if (seconds > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

// ---- file watcher: modification-time polling ------------------------------------------------------------------
namespace {
struct PollingFileWatcher final : FileWatcher {
    struct Entry {
        std::string path;
        bool isDir = false;
        std::string stamp; ///< what is compared between polls
    };
    std::vector<Entry> entries;

    static std::string stampOf(const fs::path& p) {
        std::error_code ec;
        const auto t = fs::last_write_time(p, ec);
        if (ec) return "missing";
        const auto n = fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : std::uintmax_t(0);
        // (libc++ file_time_type counts in __int128: std::to_string has no overload for it)
        return std::to_string(static_cast<long long>(t.time_since_epoch().count())) + ":" + std::to_string(static_cast<unsigned long long>(n));
    }
    static std::string stampOfEntry(const Entry& e) {
        const fs::path p = toPath(e.path);
        std::string s = stampOf(p);
        if (!e.isDir) return s;
        std::error_code ec;
        for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
            s += "|" + fromPath(it->path().filename()) + "=" + stampOf(it->path());
        return s;
    }
    bool add(std::string_view path) override {
        std::error_code ec;
        const fs::path p = toPath(path);
        if (!fs::exists(p, ec)) return false;
        Entry e{std::string(path), fs::is_directory(p, ec), {}};
        e.stamp = stampOfEntry(e);
        entries.push_back(std::move(e));
        return true;
    }
    std::vector<std::string> changed() {
        std::vector<std::string> out;
        for (Entry& e : entries) {
            std::string s = stampOfEntry(e);
            if (s != e.stamp) {
                e.stamp = std::move(s);
                out.push_back(e.path);
            }
        }
        return out;
    }
    std::vector<std::string> poll(int timeoutMs) override {
        const double until = monotonicSeconds() + double(timeoutMs < 0 ? 0 : timeoutMs) * 1e-3;
        for (;;) {
            std::vector<std::string> out = changed();
            if (!out.empty() || monotonicSeconds() >= until) return out;
            sleepSeconds(0.01);
        }
    }
};
} // namespace

std::unique_ptr<FileWatcher> createFileWatcher() { return std::make_unique<PollingFileWatcher>(); }

// ---- instance lock, dynamic libraries ---------------------------------------------------------------------------
namespace {
#if defined(_WIN32)
struct StdInstanceLock final : InstanceLock {
    HANDLE h = nullptr;
    ~StdInstanceLock() override {
        if (h) ::CloseHandle(h);
    }
};
struct StdLibrary final : Library {
    HMODULE h = nullptr;
    ~StdLibrary() override {
        if (h) ::FreeLibrary(h);
    }
    void* symbol(const char* name) const override { return reinterpret_cast<void*>(::GetProcAddress(h, name)); }
};
#else
struct StdInstanceLock final : InstanceLock {
    int fd = -1;
    ~StdInstanceLock() override {
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    }
};
struct StdLibrary final : Library {
    void* h = nullptr;
    ~StdLibrary() override {
        if (h) ::dlclose(h);
    }
    void* symbol(const char* name) const override { return ::dlsym(h, name); }
};
#endif
} // namespace

std::unique_ptr<InstanceLock> tryLockInstance(std::string_view name) {
#if defined(_WIN32)
    const std::string n = "Local\\fuse_app_" + std::string(name);
    HANDLE h = ::CreateMutexA(nullptr, TRUE, n.c_str());
    if (!h) return nullptr;
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(h);
        return nullptr;
    }
    auto l = std::make_unique<StdInstanceLock>();
    l->h = h;
    return l;
#else
    const std::string file = runtimeDir() + "/fuse_app_" + std::string(name) + "_" + env("USER", "user") + ".lock";
    const int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return nullptr;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return nullptr;
    }
    auto l = std::make_unique<StdInstanceLock>();
    l->fd = fd;
    return l;
#endif
}

std::unique_ptr<Library> loadLibrary(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (!n || !*n) continue;
#if defined(_WIN32)
        if (HMODULE h = ::LoadLibraryA(n)) {
            auto l = std::make_unique<StdLibrary>();
            l->h = h;
            return l;
        }
#else
        if (void* h = ::dlopen(n, RTLD_NOW | RTLD_LOCAL)) {
            auto l = std::make_unique<StdLibrary>();
            l->h = h;
            return l;
        }
#endif
    }
    return nullptr;
}

// ---- helper processes ---------------------------------------------------------------------------------------
std::string findExecutable(std::string_view name) {
    if (name.empty()) return {};
#if defined(_WIN32)
    const char sep = ';';
    const std::vector<std::string> exts{"", ".exe", ".cmd", ".bat"};
#else
    const char sep = ':';
    const std::vector<std::string> exts{""};
#endif
    auto runnable = [](const std::string& p) {
        std::error_code ec;
        if (!fs::is_regular_file(toPath(p), ec)) return false;
#if defined(_WIN32)
        return true;
#else
        return ::access(p.c_str(), X_OK) == 0;
#endif
    };
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos)
        return runnable(std::string(name)) ? std::string(name) : std::string();
    const std::string path = env("PATH");
    std::size_t a = 0;
    while (a <= path.size()) {
        std::size_t b = path.find(sep, a);
        if (b == std::string::npos) b = path.size();
        if (b > a) {
            for (const std::string& e : exts) {
                std::string cand = path.substr(a, b - a) + "/" + std::string(name) + e;
                if (runnable(cand)) return cand;
            }
        }
        a = b + 1;
    }
    return {};
}

#if FUSE_UI_OS_HAS_PROCESS
namespace {
void setNonBlocking(int fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
} // namespace

std::expected<ProcessResult, std::string> runProcess(std::vector<std::string> argv, std::optional<std::string> in, int timeoutMs) {
    if (argv.empty()) return std::unexpected(std::string("empty command"));
    const std::string exe = findExecutable(argv[0]);
    if (exe.empty()) return std::unexpected("not found: " + argv[0]);
    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1};
    auto closeAll = [&] {
        for (int* p : {pin, pout, perr})
            for (int i = 0; i < 2; ++i)
                if (p[i] >= 0) {
                    ::close(p[i]);
                    p[i] = -1;
                }
    };
    if (::pipe(pin) != 0 || ::pipe(pout) != 0 || ::pipe(perr) != 0) {
        closeAll();
        return std::unexpected(std::string("pipe"));
    }
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (std::string& a : argv) args.push_back(a.data());
    args.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        closeAll();
        return std::unexpected(std::string("fork"));
    }
    if (pid == 0) { // child: only async-signal-safe calls from here
        ::dup2(pin[0], 0);
        ::dup2(pout[1], 1);
        ::dup2(perr[1], 2);
        for (int* p : {pin, pout, perr}) {
            ::close(p[0]);
            ::close(p[1]);
        }
        ::execv(exe.c_str(), args.data());
        ::_exit(127);
    }
    ::close(pin[0]);
    ::close(pout[1]);
    ::close(perr[1]);
    pin[0] = pout[1] = perr[1] = -1;
    setNonBlocking(pin[1]);
    setNonBlocking(pout[0]);
    setNonBlocking(perr[0]);

    // a helper that exits before reading its stdin must not kill the caller with SIGPIPE
    struct sigaction ignore {}, old {};
    ignore.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &ignore, &old);

    ProcessResult res;
    const std::string input = in ? *in : std::string();
    std::size_t written = 0;
    if (input.empty()) {
        ::close(pin[1]);
        pin[1] = -1;
    }
    const double deadline = timeoutMs > 0 ? monotonicSeconds() + double(timeoutMs) * 1e-3 : 0.0;
    bool timedOut = false;
    while (pout[0] >= 0 || perr[0] >= 0 || pin[1] >= 0) {
        struct pollfd fds[3];
        int n = 0, iOut = -1, iErr = -1, iIn = -1;
        if (pout[0] >= 0) { fds[n] = {pout[0], POLLIN, 0}; iOut = n++; }
        if (perr[0] >= 0) { fds[n] = {perr[0], POLLIN, 0}; iErr = n++; }
        if (pin[1] >= 0) { fds[n] = {pin[1], POLLOUT, 0}; iIn = n++; }
        int wait = 100;
        if (timeoutMs > 0) {
            const double left = deadline - monotonicSeconds();
            if (left <= 0.0) {
                timedOut = true;
                break;
            }
            wait = int(std::min(left * 1e3 + 1.0, 100.0));
        }
        const int r = ::poll(fds, nfds_t(n), wait);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        auto drain = [](int& fd, std::string& into) {
            char buf[4096];
            for (;;) {
                const ssize_t k = ::read(fd, buf, sizeof buf);
                if (k > 0) {
                    into.append(buf, std::size_t(k));
                    continue;
                }
                if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    ::close(fd);
                    fd = -1;
                }
                return;
            }
        };
        if (iOut >= 0 && (fds[iOut].revents & (POLLIN | POLLHUP | POLLERR))) drain(pout[0], res.out);
        if (iErr >= 0 && (fds[iErr].revents & (POLLIN | POLLHUP | POLLERR))) drain(perr[0], res.err);
        if (iIn >= 0 && (fds[iIn].revents & (POLLOUT | POLLHUP | POLLERR))) {
            const ssize_t k = ::write(pin[1], input.data() + written, input.size() - written);
            if (k > 0) written += std::size_t(k);
            if (written >= input.size() || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                ::close(pin[1]);
                pin[1] = -1;
            }
        }
    }
    closeAll();
    int status = 0;
    if (timedOut) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        ::sigaction(SIGPIPE, &old, nullptr);
        return std::unexpected("timed out: " + argv[0]);
    }
    // the pipes are closed; the child may still be running (it closed its ends early), so honour the deadline
    for (;;) {
        const pid_t w = ::waitpid(pid, &status, timeoutMs > 0 ? WNOHANG : 0);
        if (w == pid || (w < 0 && errno != EINTR)) break;
        if (timeoutMs > 0 && monotonicSeconds() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            ::sigaction(SIGPIPE, &old, nullptr);
            return std::unexpected("timed out: " + argv[0]);
        }
        if (w == 0) sleepSeconds(0.002);
    }
    ::sigaction(SIGPIPE, &old, nullptr);
    res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return res;
}
#else
std::expected<ProcessResult, std::string> runProcess(std::vector<std::string>, std::optional<std::string>, int) {
    return std::unexpected(std::string("helper processes are not available in the std OS backend on this platform"));
}
#endif

// ---- clipboard (helper programs only) -----------------------------------------------------------------------
namespace {
enum class ClipTool { None, Wayland, X11 };
ClipTool clipTool() {
#if FUSE_UI_OS_HAS_PROCESS
    if (!env("WAYLAND_DISPLAY").empty() && !findExecutable("wl-paste").empty()) return ClipTool::Wayland;
    if (!env("DISPLAY").empty() && !findExecutable("xclip").empty()) return ClipTool::X11;
#endif
    return ClipTool::None;
}
std::expected<std::string, std::string> runTool(std::vector<std::string> argv, std::optional<std::string> in = {}) {
    auto r = runProcess(std::move(argv), std::move(in), 5000);
    if (!r) return std::unexpected(r.error());
    if (r->exitCode != 0) return std::unexpected(r->err.empty() ? std::string("helper failed") : r->err);
    return std::move(r->out);
}
const char* const kNoClipboard = "no clipboard helper (wl-clipboard or xclip)";
} // namespace

std::expected<std::string, std::string> clipboardText() {
    switch (clipTool()) {
    case ClipTool::Wayland: return runTool({"wl-paste", "--no-newline"});
    case ClipTool::X11: return runTool({"xclip", "-selection", "clipboard", "-o"});
    case ClipTool::None: break;
    }
    return std::unexpected(std::string(kNoClipboard));
}
bool setClipboardText(std::string_view text) {
    switch (clipTool()) {
    case ClipTool::Wayland: return bool(runTool({"wl-copy"}, std::string(text)));
    case ClipTool::X11: return bool(runTool({"xclip", "-selection", "clipboard", "-i"}, std::string(text)));
    case ClipTool::None: break;
    }
    return false;
}

std::expected<std::vector<std::string>, std::string> clipboardTypes() {
    std::expected<std::string, std::string> out = std::unexpected(std::string(kNoClipboard));
    switch (clipTool()) {
    case ClipTool::Wayland: out = runTool({"wl-paste", "--list-types"}); break;
    case ClipTool::X11: out = runTool({"xclip", "-selection", "clipboard", "-o", "-t", "TARGETS"}); break;
    case ClipTool::None: break;
    }
    if (!out) return std::unexpected(out.error());
    std::vector<std::string> types;
    std::size_t a = 0;
    while (a < out->size()) {
        std::size_t b = out->find('\n', a);
        if (b == std::string::npos) b = out->size();
        if (b > a) types.emplace_back(out->substr(a, b - a));
        a = b + 1;
    }
    return types;
}

std::expected<std::string, std::string> clipboardData(std::string_view mime) {
    const std::string m(mime);
    switch (clipTool()) {
    case ClipTool::Wayland: return runTool({"wl-paste", "--no-newline", "--type", m});
    case ClipTool::X11: return runTool({"xclip", "-selection", "clipboard", "-o", "-t", m});
    case ClipTool::None: break;
    }
    return std::unexpected(std::string(kNoClipboard));
}

bool setClipboardData(std::string_view mime, std::string_view data) {
    const std::string m(mime);
    switch (clipTool()) {
    case ClipTool::Wayland: return bool(runTool({"wl-copy", "--type", m}, std::string(data)));
    case ClipTool::X11: return bool(runTool({"xclip", "-selection", "clipboard", "-i", "-t", m}, std::string(data)));
    case ClipTool::None: break;
    }
    return false;
}

// ---- native file dialogs (same helper commands as the FORGE backend) -------------------------------------------
namespace {
std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}
std::string startPath(const NativeDialogSpec& s) {
    std::string p = s.directory;
    if (!s.fileName.empty()) {
        if (!p.empty() && p.back() != '/') p += '/';
        p += s.fileName;
    }
    return p;
}
[[maybe_unused]] std::string appleQuote(std::string_view t) {
    std::string o = "\"";
    for (char c : t) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}
} // namespace

std::vector<std::string> nativeFileDialogCommand(const NativeDialogSpec& s) {
    using K = NativeDialogSpec::Kind;
    std::vector<std::string> a;
    const std::string_view plat = platformName();
    if (plat == "windows" || plat == "ios" || plat == "android") return {};
    if (plat == "macos") {
        if (findExecutable("osascript").empty()) return {};
        std::string script = s.kind == K::Folder ? "choose folder" : s.kind == K::Save ? "choose file name" : "choose file";
        if (!s.title.empty()) script += " with prompt " + appleQuote(s.title);
        if (s.kind == K::Save && !s.fileName.empty()) script += " default name " + appleQuote(s.fileName);
        if (!s.directory.empty()) script += " default location POSIX file " + appleQuote(s.directory);
        if (s.kind == K::Open && s.multiple) script += " with multiple selections allowed";
        script = "set r to (" + script + ")\nif class of r is list then\nset o to \"\"\nrepeat with f in r\n"
                 "set o to o & POSIX path of f & linefeed\nend repeat\nreturn o\nend if\nreturn POSIX path of r";
        return {"osascript", "-e", script};
    }
    if (!findExecutable("zenity").empty()) {
        a = {"zenity", "--file-selection"};
        if (s.kind == K::Save) {
            a.push_back("--save");
            a.push_back("--confirm-overwrite");
        }
        if (s.kind == K::Folder) a.push_back("--directory");
        if (s.multiple && s.kind == K::Open) {
            a.push_back("--multiple");
            a.push_back("--separator=\n");
        }
        if (!s.title.empty()) a.push_back("--title=" + s.title);
        if (auto p = startPath(s); !p.empty()) a.push_back("--filename=" + p);
        for (const auto& f : s.filters) a.push_back("--file-filter=" + f.name + " | " + join(f.patterns, " "));
        return a;
    }
    if (!findExecutable("kdialog").empty()) {
        a = {"kdialog"};
        a.push_back(s.kind == K::Save ? "--getsavefilename" : s.kind == K::Folder ? "--getexistingdirectory" : "--getopenfilename");
        a.push_back(startPath(s).empty() ? std::string(".") : startPath(s));
        if (s.kind != K::Folder && !s.filters.empty()) {
            std::vector<std::string> fl;
            for (const auto& f : s.filters) fl.push_back(f.name + " (" + join(f.patterns, " ") + ")");
            a.push_back(join(fl, "\n"));
        }
        if (s.multiple && s.kind == K::Open) {
            a.push_back("--multiple");
            a.push_back("--separate-output");
        }
        if (!s.title.empty()) {
            a.push_back("--title");
            a.push_back(s.title);
        }
        return a;
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> fileDialogFilterSpec(const NativeDialogSpec& s) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& f : s.filters) {
        if (f.patterns.empty()) continue;
        out.emplace_back(f.name.empty() ? join(f.patterns, ";") : f.name, join(f.patterns, ";"));
    }
    return out;
}

#if defined(_WIN32)
NativeDialogResult win32FileDialog(const NativeDialogSpec& s); // file_dialog_win32.cpp
#endif

NativeDialogResult nativeFileDialog(const NativeDialogSpec& s) {
#if defined(_WIN32)
    return win32FileDialog(s);
#else
    NativeDialogResult res;
    auto argv = nativeFileDialogCommand(s);
    if (argv.empty()) {
        res.unavailable = true;
        return res;
    }
    auto r = runProcess(std::move(argv), {}, 0);
    if (!r) {
        res.unavailable = true;
        return res;
    }
    if (r->exitCode != 0) return res; // cancelled
    std::size_t a = 0;
    const std::string& o = r->out;
    while (a < o.size()) {
        std::size_t b = o.find('\n', a);
        if (b == std::string::npos) b = o.size();
        if (b > a) res.paths.emplace_back(o.substr(a, b - a));
        a = b + 1;
    }
    return res;
#endif
}

// ---- fd plumbing (POSIX) ----------------------------------------------------------------------------------------
#if !defined(_WIN32)
SharedBuffer& SharedBuffer::operator=(SharedBuffer&& o) noexcept {
    if (this != &o) {
        if (data) ::munmap(data, size);
        if (fd >= 0) ::close(fd);
        fd = std::exchange(o.fd, -1);
        data = std::exchange(o.data, nullptr);
        size = std::exchange(o.size, 0);
    }
    return *this;
}
SharedBuffer::~SharedBuffer() {
    if (data) ::munmap(data, size);
    if (fd >= 0) ::close(fd);
    data = nullptr;
    fd = -1;
}
std::expected<SharedBuffer, std::string> createSharedBuffer(std::size_t size) {
    int fd = -1;
#if defined(__ANDROID__)
    fd = int(::syscall(__NR_memfd_create, "fuse-ui", 1u /* MFD_CLOEXEC */)); // bionic exports memfd_create from API 30
#elif defined(__linux__) || defined(__FreeBSD__)
    fd = ::memfd_create("fuse-ui", MFD_CLOEXEC);
#endif
#if !defined(__ANDROID__) // no POSIX shm on Android
    if (fd < 0) {
        const std::string name = "/fuse-ui-" + std::to_string(::getpid()) + "-" +
                                 std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) ::shm_unlink(name.c_str());
    }
#endif
    if (fd < 0) return std::unexpected(std::string("shared memory fd"));
    if (::ftruncate(fd, off_t(size)) != 0) {
        ::close(fd);
        return std::unexpected(std::string("ftruncate"));
    }
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        ::close(fd);
        return std::unexpected(std::string("mmap"));
    }
    SharedBuffer b;
    b.fd = fd;
    b.data = p;
    b.size = size;
    return b;
}
std::expected<std::string, std::string> readMappedFd(int fd, std::size_t size) {
    void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) return std::unexpected(std::string("mmap fd"));
    std::string s(static_cast<const char*>(p), size);
    ::munmap(p, size);
    return s;
}
std::expected<std::pair<int, int>, std::string> makePipe() {
    int p[2];
    if (::pipe(p) != 0) return std::unexpected(std::string("pipe"));
    ::fcntl(p[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return std::pair{p[0], p[1]};
}
std::string readAllAndClose(int fd, int timeoutMs) {
    std::string out;
    if (fd < 0) return out;
    char buf[65536];
    for (;;) {
        struct pollfd pf {fd, POLLIN, 0};
        const int r = ::poll(&pf, 1, timeoutMs);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        const ssize_t k = ::read(fd, buf, sizeof buf);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        out.append(buf, std::size_t(k));
    }
    ::close(fd);
    return out;
}
bool writeAllAndClose(int fd, std::string_view data) {
    if (fd < 0) return false;
    std::size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        const ssize_t k = ::write(fd, data.data() + done, data.size() - done);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) {
            ok = false;
            break;
        }
        done += std::size_t(k);
    }
    ::close(fd);
    return ok;
}
void closeFd(int fd) {
    if (fd >= 0) ::close(fd);
}
#else
SharedBuffer& SharedBuffer::operator=(SharedBuffer&& o) noexcept {
    std::swap(fd, o.fd);
    std::swap(data, o.data);
    std::swap(size, o.size);
    return *this;
}
SharedBuffer::~SharedBuffer() = default;
std::expected<SharedBuffer, std::string> createSharedBuffer(std::size_t) { return std::unexpected(std::string("unsupported")); }
std::expected<std::string, std::string> readMappedFd(int, std::size_t) { return std::unexpected(std::string("unsupported")); }
std::expected<std::pair<int, int>, std::string> makePipe() { return std::unexpected(std::string("unsupported")); }
std::string readAllAndClose(int, int) { return {}; }
bool writeAllAndClose(int, std::string_view) { return false; }
void closeFd(int) {}
#endif

DisplayHdr displayHdr(void*) { return {}; }

} // namespace fuse::ui::os
