#include <fuse/script/script_hot_reload.hpp>

#include <fuse/script/script_runtime.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace fuse::script {

namespace {

struct FileStamp {
    bool ok = false;
    s64 last_write = 0;
    u64 size = 0;
};

FileStamp stamp_of(const std::string& path) {
    FileStamp stamp;
    std::error_code ec;
    const std::filesystem::path p(path);
    const auto write_time = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return stamp;
    }
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) {
        return stamp;
    }
    stamp.ok = true;
    stamp.last_write = static_cast<s64>(write_time.time_since_epoch().count());
    stamp.size = static_cast<u64>(size);
    return stamp;
}

bool hash_file(const std::string& path, u64& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    u64 hash = 1469598103934665603ull; // FNV-1a
    char buffer[4096];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            hash ^= static_cast<u8>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    out = hash;
    return true;
}

} // namespace

bool ScriptHotReload::add(const std::string& path) {
    for (const WatchedScript& watched : m_watched) {
        if (watched.path == path) {
            return false;
        }
    }
    WatchedScript watched;
    watched.path = path;
    m_watched.push_back(std::move(watched));
    return true;
}

bool ScriptHotReload::watch(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), ec)) {
        return false;
    }
    add(path);
    return true;
}

usize ScriptHotReload::watch_directory(const char* directory, const char* extension) {
    if (directory == nullptr || directory[0] == '\0') {
        return 0;
    }
    const usize before = m_watched.size();
    m_directories.emplace_back(directory, extension != nullptr ? extension : "");
    scan_directories();
    return m_watched.size() - before;
}

void ScriptHotReload::scan_directories() {
    for (const auto& [directory, extension] : m_directories) {
        std::error_code ec;
        std::filesystem::directory_iterator it(std::filesystem::path(directory), ec);
        if (ec) {
            continue;
        }
        for (const auto& entry : it) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec)) {
                continue;
            }
            if (!extension.empty() && entry.path().extension().string() != extension) {
                continue;
            }
            add(entry.path().string());
        }
    }
}

void ScriptHotReload::clear() {
    m_watched.clear();
    m_directories.clear();
    m_reloadCount = 0;
    m_failureCount = 0;
    m_lastError.clear();
}

usize ScriptHotReload::poll(ScriptRuntime& runtime) {
    if (!m_directories.empty()) {
        scan_directories();
    }

    usize reloaded = 0;
    for (WatchedScript& watched : m_watched) {
        const FileStamp stamp = stamp_of(watched.path);
        if (!stamp.ok) {
            continue; // deleted or mid-replace: keep the running version
        }
        if (watched.loaded && stamp.last_write == watched.last_write && stamp.size == watched.size) {
            continue;
        }
        u64 hash = 0;
        if (!hash_file(watched.path, hash)) {
            continue;
        }
        watched.last_write = stamp.last_write;
        watched.size = stamp.size;
        if (watched.loaded && hash == watched.content_hash) {
            continue; // touched but unchanged
        }
        watched.content_hash = hash;
        watched.loaded = true;

        const ScriptLoadResult result = runtime.load_module_file(watched.path.c_str());
        if (result.ok()) {
            ++reloaded;
            ++m_reloadCount;
        } else {
            ++m_failureCount;
            m_lastError = (result.message != nullptr) ? result.message : "reload failed";
        }
    }
    return reloaded;
}

} // namespace fuse::script
