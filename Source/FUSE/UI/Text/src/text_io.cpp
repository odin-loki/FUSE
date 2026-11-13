// File access shared by fuse_text's sources (text_internal.hpp): whole-file reads and read-only mappings with UTF-8
// paths, and name-table strings.
#include "fuse/text/text.hpp"
#include "text_internal.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace fuse::text {

namespace {
fs::path toPath(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}
} // namespace

namespace detail {
bool readFile(const std::string& utf8Path, std::vector<std::uint8_t>& out) {
    std::ifstream f(toPath(utf8Path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n <= 0) return false;
    out.resize(std::size_t(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return bool(f);
}

// Read-only file mapping (COOKED packs: pages are read on first touch, so loading a large pack costs nothing).
MappedFile::~MappedFile() { reset(); }
MappedFile::MappedFile(MappedFile&& o) noexcept
    : m_data(std::exchange(o.m_data, nullptr)), m_size(std::exchange(o.m_size, 0)), m_handle(std::exchange(o.m_handle, nullptr)) {}
MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this != &o) {
        reset();
        m_data = std::exchange(o.m_data, nullptr);
        m_size = std::exchange(o.m_size, 0);
        m_handle = std::exchange(o.m_handle, nullptr);
    }
    return *this;
}
void MappedFile::reset() {
    if (!m_data) return;
#if defined(_WIN32)
    UnmapViewOfFile(m_data);
    CloseHandle(static_cast<HANDLE>(m_handle));
#else
    munmap(const_cast<std::uint8_t*>(m_data), m_size);
#endif
    m_data = nullptr;
    m_size = 0;
    m_handle = nullptr;
}
bool MappedFile::open(const std::string& utf8Path) {
    reset();
#if defined(_WIN32)
    const HANDLE file = CreateFileW(toPath(utf8Path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    HANDLE mapping = nullptr;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0)
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file); // the mapping keeps the file open
    if (!mapping) return false;
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        return false;
    }
    m_data = static_cast<const std::uint8_t*>(view);
    m_size = std::size_t(size.QuadPart);
    m_handle = mapping;
#else
    const int fd = ::open(utf8Path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st {};
    void* view = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size > 0) view = mmap(nullptr, std::size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (view == MAP_FAILED) return false;
    m_data = static_cast<const std::uint8_t*>(view);
    m_size = std::size_t(st.st_size);
#endif
    return true;
}
} // namespace detail

namespace {
std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
void appendUtf16Be(std::string& out, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        char32_t c = be16(p + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            const char32_t lo = be16(p + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, c);
    }
}
} // namespace

// Best record of `nameId`: Windows English, any Windows, Unicode, then Mac Roman (ASCII part).
std::string detail::fontNameString(std::span<const std::uint8_t> t, std::uint16_t nameId) {
    if (t.size() < 6) return {};
    const std::uint16_t count = be16(t.data() + 2), strOff = be16(t.data() + 4);
    int bestScore = -1;
    std::string best;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t r = 6 + std::size_t(i) * 12;
        if (r + 12 > t.size()) break;
        const std::uint8_t* p = t.data() + r;
        if (be16(p + 6) != nameId) continue;
        const std::uint16_t plat = be16(p), enc = be16(p + 2), lang = be16(p + 4), len = be16(p + 8), off = be16(p + 10);
        if (std::size_t(strOff) + off + len > t.size()) continue;
        int score = -1;
        if (plat == 3 && (enc == 1 || enc == 10)) score = lang == 0x0409 ? 4 : 3;
        else if (plat == 0) score = 2;
        else if (plat == 1 && enc == 0 && lang == 0) score = 1;
        if (score <= bestScore) continue;
        bestScore = score;
        best.clear();
        const std::uint8_t* s = t.data() + strOff + off;
        if (plat == 1)
            for (std::size_t k = 0; k < len; ++k) best.push_back(s[k] < 0x80 ? char(s[k]) : '?');
        else appendUtf16Be(best, s, len);
    }
    return best;
}

} // namespace fuse::text
