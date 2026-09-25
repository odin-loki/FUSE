// FUSE Relight RL-3.4: file notifications for hot reload (see file_watcher.hpp).
#include <fuse/relight/replace/file_watcher.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace fuse::relight::replace {

namespace fs = std::filesystem;

const char* watchBackendName(WatchBackend b) {
    switch (b) {
    case WatchBackend::Auto: return "auto";
    case WatchBackend::Notify: return "notify";
    case WatchBackend::Poll: return "poll";
    }
    return "auto";
}

WatchBackend parseWatchBackend(const std::string& s) {
    if (s == "notify") {
        return WatchBackend::Notify;
    }
    if (s == "poll") {
        return WatchBackend::Poll;
    }
    return WatchBackend::Auto;
}

bool runningUnderWine() {
#if defined(_WIN32)
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

namespace {

std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool isSmallText(const fs::path& p, std::uintmax_t size) {
    if (size > (1u << 20)) {
        return false;
    }
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".usda" || ext == ".usd" || ext == ".conf" || ext == ".json" || ext == ".txt";
}

/// The poll backend's signature of one directory.
std::uint64_t directorySignature(const WatchedDir& w) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    std::error_code ec;
    if (!fs::is_directory(fs::path(w.dir), ec)) {
        return h ^ 1; // missing: a stable value different from an empty directory
    }
    std::vector<fs::path> entries;
    if (w.recursive) {
        for (fs::recursive_directory_iterator it(fs::path(w.dir), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            entries.push_back(it->path());
        }
    } else {
        for (fs::directory_iterator it(fs::path(w.dir), ec), end; !ec && it != end; it.increment(ec)) {
            entries.push_back(it->path());
        }
    }
    std::sort(entries.begin(), entries.end());
    for (const fs::path& p : entries) {
        const std::string rel = p.lexically_relative(fs::path(w.dir)).generic_string();
        h = fnv(h, rel.data(), rel.size() + 1);
        std::error_code e2;
        const bool dir = fs::is_directory(p, e2);
        h = fnv(h, &dir, sizeof dir);
        if (dir) {
            continue;
        }
        const std::uintmax_t size = fs::file_size(p, e2);
        h = fnv(h, &size, sizeof size);
        const auto t = fs::last_write_time(p, e2).time_since_epoch().count();
        h = fnv(h, &t, sizeof t);
        if (w.recursive && isSmallText(p, size)) {
            std::ifstream in(p, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            h = fnv(h, text.data(), text.size());
        }
    }
    return h;
}

} // namespace

struct DirectoryWatcher::Impl {
    enum class Kind { Poll, Inotify, Win32 } kind = Kind::Poll;
    std::uint32_t pollIntervalMs = 0;
    // poll
    std::vector<std::uint64_t> signatures;
    std::chrono::steady_clock::time_point lastPoll{};
#if defined(__linux__)
    int fd = -1;
    std::map<int, std::size_t> watches; ///< inotify watch descriptor -> watched index
    std::map<int, std::string> watchPaths;
#endif
#if defined(_WIN32)
    struct Win32Watch {
        HANDLE dir = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED ov{};
        alignas(DWORD) unsigned char buffer[16384];
        bool pending = false;
        bool recursive = true;
    };
    std::vector<std::unique_ptr<Win32Watch>> win;
#endif
};

#if defined(__linux__)
namespace {
constexpr std::uint32_t kInotifyMask = IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                       IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

void addInotifyTree(DirectoryWatcher::Impl& impl, const std::string& dir, std::size_t index, bool recursive) {
    const int wd = inotify_add_watch(impl.fd, dir.c_str(), kInotifyMask);
    if (wd >= 0) {
        impl.watches[wd] = index;
        impl.watchPaths[wd] = dir;
    }
    if (!recursive) {
        return;
    }
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2) && !it->is_symlink(e2)) {
            addInotifyTree(impl, it->path().string(), index, true);
        }
    }
}
} // namespace
#endif

#if defined(_WIN32)
namespace {
bool armWin32(DirectoryWatcher::Impl::Win32Watch& w) {
    ResetEvent(w.event);
    w.ov = OVERLAPPED{};
    w.ov.hEvent = w.event;
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    w.pending = ReadDirectoryChangesW(w.dir, w.buffer, sizeof w.buffer, w.recursive ? TRUE : FALSE, filter, nullptr,
                                      &w.ov, nullptr) != 0;
    return w.pending;
}
} // namespace
#endif

DirectoryWatcher::DirectoryWatcher(std::vector<WatchedDir> dirs, WatchBackend backend, std::uint32_t pollIntervalMs)
    : m_dirs(std::move(dirs)), m_impl(std::make_unique<Impl>()) {
    m_impl->pollIntervalMs = pollIntervalMs;
    const bool wantNotify = backend == WatchBackend::Notify || (backend == WatchBackend::Auto && !runningUnderWine());
    if (wantNotify) {
#if defined(__linux__)
        m_impl->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_impl->fd >= 0) {
            m_impl->kind = Impl::Kind::Inotify;
            for (std::size_t i = 0; i < m_dirs.size(); ++i) {
                addInotifyTree(*m_impl, m_dirs[i].dir, i, m_dirs[i].recursive);
            }
        }
#elif defined(_WIN32)
        bool ok = true;
        for (const WatchedDir& d : m_dirs) {
            auto w = std::make_unique<Impl::Win32Watch>();
            w->recursive = d.recursive;
            const std::wstring wide = fs::path(d.dir).wstring();
            w->dir = CreateFileW(wide.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            w->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (w->dir != INVALID_HANDLE_VALUE && w->event) {
                armWin32(*w); // a directory that cannot be armed reports nothing (missing directories)
            } else if (w->dir == INVALID_HANDLE_VALUE) {
                std::error_code ec;
                ok = ok && !fs::exists(fs::path(d.dir), ec); // an existing directory that cannot be opened: poll
            }
            m_impl->win.push_back(std::move(w));
        }
        if (ok) {
            m_impl->kind = Impl::Kind::Win32;
        } else {
            for (auto& w : m_impl->win) {
                if (w->pending) {
                    CancelIoEx(w->dir, &w->ov);
                    DWORD n = 0;
                    GetOverlappedResult(w->dir, &w->ov, &n, TRUE);
                }
                if (w->dir != INVALID_HANDLE_VALUE) {
                    CloseHandle(w->dir);
                }
                if (w->event) {
                    CloseHandle(w->event);
                }
            }
            m_impl->win.clear();
        }
#endif
    }
    if (m_impl->kind == Impl::Kind::Poll) {
        for (const WatchedDir& d : m_dirs) {
            m_impl->signatures.push_back(directorySignature(d));
        }
        m_impl->lastPoll = std::chrono::steady_clock::now();
    }
}

DirectoryWatcher::~DirectoryWatcher() {
#if defined(__linux__)
    if (m_impl->fd >= 0) {
        close(m_impl->fd);
    }
#endif
#if defined(_WIN32)
    for (auto& w : m_impl->win) {
        if (w->pending) {
            CancelIoEx(w->dir, &w->ov);
            DWORD n = 0;
            GetOverlappedResult(w->dir, &w->ov, &n, TRUE);
        }
        if (w->dir != INVALID_HANDLE_VALUE) {
            CloseHandle(w->dir);
        }
        if (w->event) {
            CloseHandle(w->event);
        }
    }
#endif
}

const char* DirectoryWatcher::backend() const {
    switch (m_impl->kind) {
    case Impl::Kind::Inotify: return "inotify";
    case Impl::Kind::Win32: return "win32";
    case Impl::Kind::Poll: return "poll";
    }
    return "poll";
}

std::vector<std::size_t> DirectoryWatcher::poll(bool force) {
    std::set<std::size_t> changed;
    switch (m_impl->kind) {
    case Impl::Kind::Poll: {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_impl->lastPoll).count();
        if (!force && m_impl->pollIntervalMs > 0 && elapsed < std::int64_t(m_impl->pollIntervalMs)) {
            break;
        }
        m_impl->lastPoll = now;
        for (std::size_t i = 0; i < m_dirs.size(); ++i) {
            const std::uint64_t s = directorySignature(m_dirs[i]);
            if (s != m_impl->signatures[i]) {
                m_impl->signatures[i] = s;
                changed.insert(i);
            }
        }
        break;
    }
    case Impl::Kind::Inotify: {
#if defined(__linux__)
        alignas(inotify_event) char buf[16384];
        for (;;) {
            const ssize_t n = read(m_impl->fd, buf, sizeof buf);
            if (n <= 0) {
                break; // EAGAIN: drained
            }
            for (ssize_t off = 0; off < n;) {
                const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
                off += ssize_t(sizeof(inotify_event) + ev->len);
                if (ev->mask & IN_Q_OVERFLOW) {
                    for (std::size_t i = 0; i < m_dirs.size(); ++i) {
                        changed.insert(i);
                    }
                    continue;
                }
                const auto it = m_impl->watches.find(ev->wd);
                if (it == m_impl->watches.end()) {
                    continue;
                }
                changed.insert(it->second);
                if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO)) && ev->len > 0 &&
                    m_dirs[it->second].recursive) {
                    addInotifyTree(*m_impl, m_impl->watchPaths[ev->wd] + "/" + ev->name, it->second, true);
                }
                if (ev->mask & IN_IGNORED) {
                    m_impl->watchPaths.erase(ev->wd);
                    m_impl->watches.erase(it);
                }
            }
        }
#endif
        break;
    }
    case Impl::Kind::Win32: {
#if defined(_WIN32)
        for (std::size_t i = 0; i < m_impl->win.size(); ++i) {
            auto& w = *m_impl->win[i];
            if (!w.pending) {
                continue;
            }
            DWORD n = 0;
            if (GetOverlappedResult(w.dir, &w.ov, &n, FALSE)) {
                changed.insert(i); // n == 0: the buffer overflowed; still a change
                armWin32(w);
            } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
                changed.insert(i);
                w.pending = false; // the directory went away
            }
        }
#endif
        break;
    }
    }
    return {changed.begin(), changed.end()};
}

} // namespace fuse::relight::replace
