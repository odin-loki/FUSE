#include <fuse/renderer/shader/shader_watch.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <iterator>
#include <string>

namespace fuse::renderer {

namespace {

struct FileStamp {
    u64 mtime = 0;
    u64 size = 0;
};

FileStamp queryStamp(const char* path) {
    FileStamp stamp{};
    if (path == nullptr) {
        return stamp;
    }

#if defined(_WIN32)
    // UTF-16 path in a stack buffer: pollChanged runs every frame and must not allocate (heap only for long paths).
    wchar_t local[1024];
    std::wstring longPath;
    wchar_t* wide = local;
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (needed <= 0) {
        return stamp;
    }
    if (needed > static_cast<int>(std::size(local))) {
        longPath.resize(static_cast<std::size_t>(needed));
        wide = longPath.data();
    }
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, needed) <= 0) {
        return stamp;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &attributes)) {
        return stamp;
    }

    stamp.mtime = (static_cast<u64>(attributes.ftLastWriteTime.dwHighDateTime) << 32u) |
                  static_cast<u64>(attributes.ftLastWriteTime.dwLowDateTime);
    stamp.size = (static_cast<u64>(attributes.nFileSizeHigh) << 32u) |
                 static_cast<u64>(attributes.nFileSizeLow);
#else
    struct stat fileStat {};
    if (::stat(path, &fileStat) != 0) {
        return stamp;
    }

    stamp.size = static_cast<u64>(fileStat.st_size);
#if defined(__APPLE__)
    stamp.mtime = static_cast<u64>(fileStat.st_mtimespec.tv_sec) * 1000000000ull +
                  static_cast<u64>(fileStat.st_mtimespec.tv_nsec);
#elif defined(__linux__)
    stamp.mtime = static_cast<u64>(fileStat.st_mtim.tv_sec) * 1000000000ull +
                  static_cast<u64>(fileStat.st_mtim.tv_nsec);
#else
    stamp.mtime = static_cast<u64>(fileStat.st_mtime);
#endif
#endif
    return stamp;
}

} // namespace

bool ShaderFileWatch::watch(const char* path) {
    if (path == nullptr) {
        return false;
    }

    const FileStamp stamp = queryStamp(path);
    m_entries.push_back(Entry{std::string(path), stamp.mtime, stamp.size});
    return true;
}

u32 ShaderFileWatch::pollChanged() {
    m_lastChanged.clear();
    u32 changed = 0;
    for (Entry& entry : m_entries) {
        const FileStamp stamp = queryStamp(entry.path.c_str());
        if (stamp.mtime != entry.mtime || stamp.size != entry.size) {
            m_lastChanged.push_back(entry.path);
            ++changed;
            entry.mtime = stamp.mtime;
            entry.size = stamp.size;
        }
    }
    return changed;
}

u32 ShaderFileWatch::watchedCount() const {
    return static_cast<u32>(m_entries.size());
}

u32 ShaderFileWatch::lastChangedCount() const {
    return static_cast<u32>(m_lastChanged.size());
}

const char* ShaderFileWatch::lastChangedPath(u32 index) const {
    if (index >= m_lastChanged.size()) {
        return nullptr;
    }
    return m_lastChanged[index].c_str();
}

} // namespace fuse::renderer
