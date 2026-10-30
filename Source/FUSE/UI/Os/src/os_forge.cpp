// fuse::ui::os on FORGE (forge::os). Everything OS-specific here is FORGE's; this file only adapts signatures.
#include "fuse/ui/os.hpp"

#include <forge/os/clipboard.hpp>
#include <forge/os/file_watch.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>
#include <forge/os/process.hpp>

namespace fuse::ui::os {
namespace fo = forge::os;

std::string env(std::string_view name, std::string_view fallback) {
    std::string v = fo::getenv(std::string(name));
    return v.empty() ? std::string(fallback) : v;
}

std::string configDir(std::string_view app) {
    const std::string a(app);
    if constexpr (fo::current() == fo::platform::windows) {
        return env("APPDATA", ".") + "\\" + a;
    } else if constexpr (fo::current() == fo::platform::macos) {
        return env("HOME", ".") + "/Library/Application Support/" + a;
    } else {
        if (std::string x = env("XDG_CONFIG_HOME"); !x.empty()) return x + "/" + a;
        return env("HOME", ".") + "/.config/" + a;
    }
}

std::string runtimeDir() {
    if (std::string r = env("XDG_RUNTIME_DIR"); !r.empty()) return r;
    return fo::temp_dir();
}

std::expected<std::string, std::string> readFile(std::string_view path) {
    auto r = fo::read_file(std::string(path));
    if (!r) return std::unexpected(r.err.what);
    return std::move(r.value);
}

std::expected<void, std::string> writeFileAtomic(std::string_view path, std::string_view data) {
    const std::string p(path);
    if (const std::size_t slash = p.find_last_of("/\\"); slash != std::string::npos && slash > 0)
        if (auto s = fo::fs::make_dirs(p.substr(0, slash)); !s) return std::unexpected(s.err.what);
    if (auto s = fo::fs::write_file_atomic(p, data, 0600); !s) return std::unexpected(s.err.what);
    return {};
}

bool isDirectory(std::string_view path) { return fo::fs::is_dir(std::string(path)); }
bool exists(std::string_view path) { return fo::exists(std::string(path)); }
std::string absolutePath(std::string_view path) {
    auto r = fo::fs::absolute(std::string(path));
    std::string a = r ? r.value : fo::absolute(std::string(path));
    for (char& c : a)
        if (c == '\\') c = '/';
    if (a.size() > 1 && a.back() == '/') a.pop_back();
    return a;
}
std::expected<std::vector<DirEntry>, std::string> listDirectory(std::string_view path) {
    auto r = fo::list_dir(std::string(path));
    if (!r) return std::unexpected(r.err.what);
    std::vector<DirEntry> out;
    out.reserve(r.value.size());
    for (auto& e : r.value)
        if (e.name != "." && e.name != "..") out.push_back({std::move(e.name), e.is_dir});
    return out;
}

double monotonicSeconds() {
    static const std::uint64_t t0 = fo::monotonic_ns();
    return double(fo::monotonic_ns() - t0) * 1e-9;
}
void sleepSeconds(double seconds) {
    if (seconds > 0.0) fo::sleep_ns(std::uint64_t(seconds * 1e9));
}

namespace {
struct ForgeInstanceLock final : InstanceLock {
    fo::fs::file_lock lock;
};
struct ForgeLibrary final : Library {
    fo::library lib;
    void* symbol(const char* name) const override { return lib.symbol(name); }
};
struct ForgeFileWatcher final : FileWatcher {
    fo::file_watcher w;
    bool add(std::string_view path) override { return bool(w.add(std::string(path))); }
    std::vector<std::string> poll(int timeoutMs) override {
        std::vector<std::string> out;
        for (auto& e : w.poll(timeoutMs)) {
            bool dup = false;
            for (const auto& o : out) dup = dup || o == e.path;
            if (!dup) out.push_back(std::move(e.path));
        }
        return out;
    }
};
} // namespace

std::unique_ptr<FileWatcher> createFileWatcher() { return std::make_unique<ForgeFileWatcher>(); }

std::unique_ptr<InstanceLock> tryLockInstance(std::string_view name) {
    std::string file = runtimeDir() + "/fuse_app_" + std::string(name);
    if constexpr (fo::current() != fo::platform::windows) file += "_" + env("USER", "user");
    auto r = fo::fs::file_lock::acquire(file + ".lock", /*exclusive=*/true, /*wait=*/false);
    if (!r) return nullptr;
    auto l = std::make_unique<ForgeInstanceLock>();
    l->lock = std::move(r.value);
    return l;
}

std::unique_ptr<Library> loadLibrary(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        auto r = fo::library::open(n);
        if (!r) continue;
        auto l = std::make_unique<ForgeLibrary>();
        l->lib = std::move(r.value);
        return l;
    }
    return nullptr;
}

std::expected<std::string, std::string> clipboardText() {
    auto r = fo::clipboard::get();
    if (!r) return std::unexpected(r.err.what);
    return std::move(r.value);
}
bool setClipboardText(std::string_view text) { return bool(fo::clipboard::set(std::string(text))); }


// ---- rich clipboard -------------------------------------------------------------------------------------------
namespace {
enum class ClipTool { None, Wayland, X11 };
ClipTool clipTool() {
    if (!fo::getenv("WAYLAND_DISPLAY").empty() && !fo::find_executable("wl-paste").empty()) return ClipTool::Wayland;
    if (!fo::getenv("DISPLAY").empty() && !fo::find_executable("xclip").empty()) return ClipTool::X11;
    return ClipTool::None;
}
std::expected<std::string, std::string> runTool(std::vector<std::string> argv, std::optional<std::string> in = {}) {
    fo::spawn_spec sp;
    sp.argv = std::move(argv);
    sp.stdin_data = std::move(in);
    sp.timeout_ms = 5000;
    auto r = fo::spawn_wait(sp);
    if (!r) return std::unexpected(r.err.what);
    if (r->exit_code != 0) return std::unexpected(r->err.empty() ? std::string("helper failed") : r->err);
    return std::move(r->out);
}
} // namespace

std::expected<std::vector<std::string>, std::string> clipboardTypes() {
    std::expected<std::string, std::string> out = std::unexpected(std::string("no rich clipboard helper"));
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
    std::string m(mime);
    switch (clipTool()) {
    case ClipTool::Wayland: return runTool({"wl-paste", "--no-newline", "--type", m});
    case ClipTool::X11: return runTool({"xclip", "-selection", "clipboard", "-o", "-t", m});
    case ClipTool::None: break;
    }
    if (mime == "text/plain" || mime == "text/plain;charset=utf-8") return clipboardText();
    return std::unexpected(std::string("no rich clipboard helper"));
}

bool setClipboardData(std::string_view mime, std::string_view data) {
    std::string m(mime);
    switch (clipTool()) {
    case ClipTool::Wayland: return bool(runTool({"wl-copy", "--type", m}, std::string(data)));
    case ClipTool::X11: return bool(runTool({"xclip", "-selection", "clipboard", "-i", "-t", m}, std::string(data)));
    case ClipTool::None: break;
    }
    if (mime == "text/plain" || mime == "text/plain;charset=utf-8") return setClipboardText(data);
    return false;
}

// ---- native file dialogs --------------------------------------------------------------------------------------
namespace {
std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) s += sep; s += v[i]; }
    return s;
}
std::string startPath(const NativeDialogSpec& s) {
    std::string p = s.directory;
    if (!s.fileName.empty()) { if (!p.empty() && p.back() != '/') p += '/'; p += s.fileName; }
    return p;
}
std::string appleQuote(std::string_view t) {
    std::string o = "\"";
    for (char c : t) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}
} // namespace

std::vector<std::string> nativeFileDialogCommand(const NativeDialogSpec& s) {
    using K = NativeDialogSpec::Kind;
    std::vector<std::string> a;
    switch (fo::current()) {
    case fo::platform::macos: {
        if (fo::find_executable("osascript").empty()) return {};
        std::string verb = s.kind == K::Folder ? "choose folder" : s.kind == K::Save ? "choose file name" : "choose file";
        std::string script = verb;
        if (!s.title.empty()) script += " with prompt " + appleQuote(s.title);
        if (s.kind == K::Save && !s.fileName.empty()) script += " default name " + appleQuote(s.fileName);
        if (!s.directory.empty()) script += " default location POSIX file " + appleQuote(s.directory);
        if (s.kind == K::Open && s.multiple) script += " with multiple selections allowed";
        // POSIX paths, one per line.
        script = "set r to (" + script + ")\nif class of r is list then\nset o to \"\"\nrepeat with f in r\n"
                 "set o to o & POSIX path of f & linefeed\nend repeat\nreturn o\nend if\nreturn POSIX path of r";
        return {"osascript", "-e", script};
    }
    case fo::platform::windows:
    case fo::platform::ios:
    case fo::platform::android: return {}; // Win32: IFileDialog (file_dialog_win32.cpp, no child process); mobile: G4
    default: break;
    }
    if (!fo::find_executable("zenity").empty()) {
        a = {"zenity", "--file-selection"};
        if (s.kind == K::Save) { a.push_back("--save"); a.push_back("--confirm-overwrite"); }
        if (s.kind == K::Folder) a.push_back("--directory");
        if (s.multiple && s.kind == K::Open) { a.push_back("--multiple"); a.push_back("--separator=\n"); }
        if (!s.title.empty()) a.push_back("--title=" + s.title);
        if (auto p = startPath(s); !p.empty()) a.push_back("--filename=" + p);
        for (const auto& f : s.filters) a.push_back("--file-filter=" + f.name + " | " + join(f.patterns, " "));
        return a;
    }
    if (!fo::find_executable("kdialog").empty()) {
        a = {"kdialog"};
        a.push_back(s.kind == K::Save ? "--getsavefilename" : s.kind == K::Folder ? "--getexistingdirectory" : "--getopenfilename");
        a.push_back(startPath(s).empty() ? std::string(".") : startPath(s));
        if (s.kind != K::Folder && !s.filters.empty()) {
            std::vector<std::string> fs;
            for (const auto& f : s.filters) fs.push_back(f.name + " (" + join(f.patterns, " ") + ")");
            a.push_back(join(fs, "\n"));
        }
        if (s.multiple && s.kind == K::Open) { a.push_back("--multiple"); a.push_back("--separate-output"); }
        if (!s.title.empty()) { a.push_back("--title"); a.push_back(s.title); }
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
#endif
    NativeDialogResult res;
    auto argv = nativeFileDialogCommand(s);
    if (argv.empty()) { res.unavailable = true; return res; }
    fo::spawn_spec sp;
    sp.argv = std::move(argv);
    auto r = fo::spawn_wait(sp);
    if (!r) { res.unavailable = true; return res; }
    if (r->exit_code != 0) return res; // cancelled
    std::size_t a = 0;
    const std::string& o = r->out;
    while (a < o.size()) {
        std::size_t b = o.find('\n', a);
        if (b == std::string::npos) b = o.size();
        if (b > a) res.paths.emplace_back(o.substr(a, b - a));
        a = b + 1;
    }
    return res;
}


// ---- fd plumbing (Wayland) ------------------------------------------------------------------------------------
// FORGE gap: forge::os::shared_memory is name-based and does not expose its fd, and forge::os::mapping cannot map
// an inherited fd, so the anonymous-fd buffer and fd mapping below use POSIX directly (memfd_create where present,
// else shm_open + unlink). Pipes and fd reads/writes use forge::os::pipe_end.
#if !defined(_WIN32)
} // namespace fuse::ui::os
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#include <forge/os/pipe.hpp>
namespace fuse::ui::os {
SharedBuffer& SharedBuffer::operator=(SharedBuffer&& o) noexcept {
    if (this != &o) {
        this->~SharedBuffer();
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
    fd = int(::syscall(__NR_memfd_create, "fuse-ui", MFD_CLOEXEC)); // bionic only exports memfd_create from API 30
#elif defined(__linux__) || defined(__FreeBSD__)
    fd = ::memfd_create("fuse-ui", MFD_CLOEXEC);
#endif
#if !defined(__ANDROID__) // no POSIX shm on Android
    if (fd < 0) {
        std::string name = "/fuse-ui-" + std::to_string(fo::pid()) + "-" + std::to_string(fo::monotonic_ns());
        fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) ::shm_unlink(name.c_str());
    }
#endif
    if (fd < 0) return std::unexpected(std::string("shared memory fd"));
    if (::ftruncate(fd, off_t(size)) != 0) { ::close(fd); return std::unexpected(std::string("ftruncate")); }
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); return std::unexpected(std::string("mmap")); }
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
    auto r = fo::make_pipe();
    if (!r) return std::unexpected(r.err.what);
    int a = ::dup(r->read.native()), b = ::dup(r->write.native()); // pipe_end closes its own fds
    if (a >= 0) ::fcntl(a, F_SETFD, FD_CLOEXEC);
    if (b >= 0) ::fcntl(b, F_SETFD, FD_CLOEXEC);
    return std::pair{a, b};
}
std::string readAllAndClose(int fd, int timeoutMs) {
    fo::pipe_end e(fd);
    std::string out;
    for (;;) {
        std::string c = e.read_some(65536, timeoutMs);
        if (c.empty()) break;
        out += c;
    }
    return out;
}
bool writeAllAndClose(int fd, std::string_view data) {
    fo::pipe_end e(fd);
    return bool(e.write_all(data));
}
void closeFd(int fd) { if (fd >= 0) ::close(fd); }
#else
SharedBuffer& SharedBuffer::operator=(SharedBuffer&& o) noexcept { std::swap(fd, o.fd); std::swap(data, o.data); std::swap(size, o.size); return *this; }
SharedBuffer::~SharedBuffer() = default;
std::expected<SharedBuffer, std::string> createSharedBuffer(std::size_t) { return std::unexpected(std::string("unsupported")); }
std::expected<std::string, std::string> readMappedFd(int, std::size_t) { return std::unexpected(std::string("unsupported")); }
std::expected<std::pair<int, int>, std::string> makePipe() { return std::unexpected(std::string("unsupported")); }
std::string readAllAndClose(int, int) { return {}; }
bool writeAllAndClose(int, std::string_view) { return false; }
void closeFd(int) {}
#endif

} // namespace fuse::ui::os

// ---- desktop helpers (runProcess / findExecutable / platformName) --------------------------------------------------
namespace fuse::ui::os {
std::expected<ProcessResult, std::string> runProcess(std::vector<std::string> argv, std::optional<std::string> in, int timeoutMs) {
    if (argv.empty()) return std::unexpected(std::string("empty command"));
    forge::os::spawn_spec sp;
    sp.argv = std::move(argv);
    sp.stdin_data = std::move(in);
    if (timeoutMs > 0) sp.timeout_ms = timeoutMs;
    auto r = forge::os::spawn_wait(sp);
    if (!r) return std::unexpected(r.err.what);
    return ProcessResult{r->exit_code, std::move(r->out), std::move(r->err)};
}
std::string findExecutable(std::string_view name) { return forge::os::find_executable(std::string(name)); }
std::string_view platformName() {
    using P = forge::os::platform;
    switch (forge::os::current()) {
    case P::windows: return "windows";
    case P::linux: return "linux";
    case P::android: return "android";
    case P::macos: return "macos";
    case P::ios: return "ios";
    case P::freebsd: return "freebsd";
    default: return "unknown";
    }
}
} // namespace fuse::ui::os
