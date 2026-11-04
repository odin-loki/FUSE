// fuse_app platform services, X11 / POSIX: zenity dialogs and message boxes, xdg-open for URLs and folders, the
// theme from $FUSE_UI_THEME / $GTK_THEME (no desktop-portal client here).
#if !defined(_WIN32)
#include "platform_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fuse::app {

const char* platformErrorName(PlatformError e) {
    switch (e) {
    case PlatformError::Cancelled: return "cancelled";
    case PlatformError::Unavailable: return "unavailable";
    case PlatformError::InvalidArgument: return "invalid argument";
    case PlatformError::Failed: return "failed";
    }
    return "?";
}

namespace {
bool onPath(const char* tool) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    for (std::size_t a = 0; a <= p.size();) {
        const std::size_t b = std::min(p.find(':', a), p.size());
        const std::string f = p.substr(a, b - a) + "/" + tool;
        if (::access(f.c_str(), X_OK) == 0) return true;
        a = b + 1;
    }
    return false;
}
/// Runs `argv` and waits; stdout into `out`. -1 when it cannot start.
int run(std::vector<std::string> args, std::string* out) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    int fds[2] = {-1, -1};
    if (out && ::pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out) {
        posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
    }
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (out) ::close(fds[1]);
    if (rc != 0) {
        if (out) ::close(fds[0]);
        return -1;
    }
    if (out) {
        char buf[4096];
        for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;) out->append(buf, std::size_t(n));
        ::close(fds[0]);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
std::expected<std::vector<std::string>, PlatformError> zenityFiles(const FileDialogDesc& d, bool save, bool folder) {
    if (!onPath("zenity")) return std::unexpected(PlatformError::Unavailable);
    std::vector<std::string> a{"zenity", "--file-selection"};
    if (save) a.push_back("--save");
    if (folder) a.push_back("--directory");
    if (d.multiple && !save && !folder) { a.push_back("--multiple"); a.push_back("--separator=\n"); }
    if (!d.title.empty()) a.push_back("--title=" + std::string(d.title));
    std::string start(d.folder);
    if (!start.empty() && start.back() != '/') start += '/';
    start += d.fileName;
    if (!start.empty()) a.push_back("--filename=" + start);
    for (const FileFilter& f : d.filters) {
        std::string pats(f.patterns);
        std::replace(pats.begin(), pats.end(), ';', ' ');
        a.push_back("--file-filter=" + std::string(f.name) + " | " + pats);
    }
    std::string out;
    const int rc = run(std::move(a), &out);
    if (rc < 0) return std::unexpected(PlatformError::Unavailable);
    if (rc != 0) return std::unexpected(PlatformError::Cancelled);
    std::vector<std::string> paths;
    for (std::size_t s = 0; s < out.size();) {
        const std::size_t e = std::min(out.find('\n', s), out.size());
        if (e > s) paths.push_back(out.substr(s, e - s));
        s = e + 1;
    }
    if (paths.empty()) return std::unexpected(PlatformError::Cancelled);
    return paths;
}
} // namespace

namespace detail {
SystemTheme posixQueryTheme(const NativeOptions& o, std::string_view gtkThemeName) {
    if (o.themeQuery) return o.themeQuery(o.themeQueryUser);
    SystemTheme t;
    auto darkName = [](std::string_view n) { // "Adwaita-dark", "Breeze:dark", "Yaru-dark" ...
        std::string l(n);
        for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
        return l.find("dark") != std::string::npos;
    };
    if (const char* e = std::getenv("FUSE_UI_THEME"); e && *e) t.dark = std::strcmp(e, "light") != 0;
    else if (const char* g = std::getenv("GTK_THEME"); g && *g) t.dark = darkName(g);
    else if (!gtkThemeName.empty()) t.dark = darkName(gtkThemeName);
    if (const char* e = std::getenv("FUSE_UI_REDUCED_MOTION"); e && *e == '1') t.reducedMotion = true;
    return t;
}
SystemTheme queryTheme(const NativeOptions& o) { return posixQueryTheme(o, {}); }

std::vector<PumpWatch>& pumpWatches() {
    static std::vector<PumpWatch> w;
    return w;
}
void addPumpWatch(int fd, void (*fn)(void*), void* user) { pumpWatches().push_back({fd, fn, user}); }
void removePumpWatch(void* user) { std::erase_if(pumpWatches(), [user](const PumpWatch& w) { return w.user == user; }); }
} // namespace detail

std::expected<std::vector<std::string>, PlatformError> openFileDialog(const FileDialogDesc& d) { return zenityFiles(d, false, false); }
std::expected<std::string, PlatformError> saveFileDialog(const FileDialogDesc& d) {
    auto r = zenityFiles(d, true, false);
    if (!r) return std::unexpected(r.error());
    return std::move(r->front());
}
std::expected<std::string, PlatformError> pickFolderDialog(const FileDialogDesc& d) {
    auto r = zenityFiles(d, false, true);
    if (!r) return std::unexpected(r.error());
    return std::move(r->front());
}
std::expected<MessageResult, PlatformError> messageBox(std::string_view title, std::string_view text, MessageIcon icon,
                                                       MessageButtons buttons, unsigned long long) {
    if (!onPath("zenity")) return std::unexpected(PlatformError::Unavailable);
    const bool ask = buttons != MessageButtons::Ok;
    std::vector<std::string> a{"zenity", ask ? "--question"
                                         : icon == MessageIcon::Error   ? "--error"
                                         : icon == MessageIcon::Warning ? "--warning"
                                                                        : "--info"};
    a.push_back("--title=" + std::string(title));
    a.push_back("--text=" + std::string(text));
    if (buttons == MessageButtons::OkCancel) { a.push_back("--ok-label=OK"); a.push_back("--cancel-label=Cancel"); }
    if (buttons == MessageButtons::YesNoCancel) a.push_back("--extra-button=Cancel");
    std::string out;
    const int rc = run(std::move(a), &out);
    if (rc < 0) return std::unexpected(PlatformError::Unavailable);
    if (!out.empty() && out.rfind("Cancel", 0) == 0) return MessageResult::Cancel;
    switch (buttons) {
    case MessageButtons::Ok: return MessageResult::Ok;
    case MessageButtons::OkCancel: return rc == 0 ? MessageResult::Ok : MessageResult::Cancel;
    default: return rc == 0 ? MessageResult::Yes : MessageResult::No;
    }
}

bool isOpenableUrl(std::string_view url) {
    auto starts = [&](std::string_view p) {
        if (url.size() < p.size()) return false;
        for (std::size_t i = 0; i < p.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(url[i])) != p[i]) return false;
        return true;
    };
    if (url.find_first_of("\r\n\"") != std::string_view::npos) return false;
    return (starts("http://") && url.size() > 7) || (starts("https://") && url.size() > 8) || (starts("mailto:") && url.size() > 7) ||
           (starts("file:") && url.size() > 5);
}
std::expected<void, PlatformError> openUrl(std::string_view url) {
    if (!isOpenableUrl(url)) return std::unexpected(PlatformError::InvalidArgument);
    if (!onPath("xdg-open")) return std::unexpected(PlatformError::Unavailable);
    if (run({"xdg-open", std::string(url)}, nullptr) != 0) return std::unexpected(PlatformError::Failed);
    return {};
}
std::expected<void, PlatformError> revealInFileManager(std::string_view path) {
    if (path.empty()) return std::unexpected(PlatformError::InvalidArgument);
    struct stat st{};
    const std::string p(path);
    if (::stat(p.c_str(), &st) != 0) return std::unexpected(PlatformError::Failed);
    std::string dir = p;
    if (!S_ISDIR(st.st_mode)) dir = p.substr(0, p.find_last_of('/') == std::string::npos ? 0 : p.find_last_of('/'));
    if (dir.empty()) dir = ".";
    if (!onPath("xdg-open")) return std::unexpected(PlatformError::Unavailable);
    if (run({"xdg-open", dir}, nullptr) != 0) return std::unexpected(PlatformError::Failed);
    return {};
}

} // namespace fuse::app
#endif
