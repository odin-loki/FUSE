// System font discovery (FontCatalog) and script classification. Reads only the table directory and the name /
// OS/2 / head tables of each file, so scanning a few hundred installed fonts stays cheap.
#include "fuse/text/text.hpp"
#include "text_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace fuse::text {

namespace {
fs::path toPath(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}
std::string fromPath(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
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
} // namespace detail

// ---- Scripts -------------------------------------------------------------------------------------------------------
Script scriptOf(char32_t c) {
    if (c < 0x80) return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ? Script::Latin : Script::Common;
    if (c < 0x0250) return (c == 0xD7 || c == 0xF7 || c < 0xC0) ? Script::Common : Script::Latin;
    if (c >= 0x0370 && c <= 0x03FF) return Script::Greek;
    if (c >= 0x1F00 && c <= 0x1FFF) return Script::Greek;
    if ((c >= 0x0400 && c <= 0x052F) || (c >= 0x2DE0 && c <= 0x2DFF) || (c >= 0xA640 && c <= 0xA69F)) return Script::Cyrillic;
    if ((c >= 0x0590 && c <= 0x05FF) || (c >= 0xFB1D && c <= 0xFB4F)) return Script::Hebrew;
    if ((c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0x08A0 && c <= 0x08FF) ||
        (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF))
        return Script::Arabic;
    if ((c >= 0x0900 && c <= 0x097F) || (c >= 0xA8E0 && c <= 0xA8FF)) return Script::Devanagari;
    if (c >= 0x0E00 && c <= 0x0E7F) return Script::Thai;
    if ((c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) || (c >= 0xAC00 && c <= 0xD7AF)) return Script::Hangul;
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9F)) return Script::Kana;
    if ((c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x3134F))
        return Script::Han;
    if ((c >= 0x1E00 && c <= 0x1EFF) || (c >= 0x2C60 && c <= 0x2C7F) || (c >= 0xA720 && c <= 0xA7FF)) return Script::Latin;
    if ((c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x1F000 && c <= 0x1F2FF)) return Script::Emoji;
    if ((c >= 0x2190 && c <= 0x2BFF) || (c >= 0x1F800 && c <= 0x1F8FF)) return Script::Symbols;
    return Script::Common;
}

const char* scriptName(Script s) {
    static const char* const kNames[] = {"Common", "Latin", "Greek", "Cyrillic", "Arabic", "Hebrew", "Devanagari",
                                         "Thai", "Han", "Kana", "Hangul", "Emoji", "Symbols"};
    return kNames[std::size_t(s)];
}

namespace {
bool rangeBit(const FontFaceInfo& f, int bit) { return (f.unicodeRanges[bit / 32] >> (bit % 32)) & 1u; }
bool containsCi(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                                        std::tolower(static_cast<unsigned char>(needle[k])))
            ++k;
        if (k == needle.size()) return true;
    }
    return false;
}
bool equalCi(std::string_view a, std::string_view b) { return a.size() == b.size() && containsCi(a, b); }
} // namespace

bool FontFaceInfo::covers(Script s) const {
    switch (s) {
    case Script::Common:
    case Script::Latin: return rangeBit(*this, 0);
    case Script::Greek: return rangeBit(*this, 7);
    case Script::Cyrillic: return rangeBit(*this, 9);
    case Script::Hebrew: return rangeBit(*this, 11);
    case Script::Arabic: return rangeBit(*this, 13);
    case Script::Devanagari: return rangeBit(*this, 15);
    case Script::Thai: return rangeBit(*this, 24);
    case Script::Kana: return rangeBit(*this, 49) || rangeBit(*this, 50);
    case Script::Hangul: return rangeBit(*this, 56);
    case Script::Han: return rangeBit(*this, 59);
    case Script::Emoji: return containsCi(family, "emoji");
    case Script::Symbols: return containsCi(family, "symbol") || (rangeBit(*this, 46) && rangeBit(*this, 47));
    }
    return false;
}

// ---- Table parsing -------------------------------------------------------------------------------------------------
namespace {
std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

bool readAt(std::ifstream& f, std::uint64_t off, std::size_t len, std::vector<std::uint8_t>& buf) {
    buf.resize(len);
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(len));
    return std::size_t(f.gcount()) == len;
}

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

// Best record of `nameId`: Windows English, any Windows, Unicode, then Mac Roman (ASCII part).
std::string nameString(const std::vector<std::uint8_t>& t, std::uint16_t nameId) {
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

bool hasFontExtension(const fs::path& p) {
    std::string e = fromPath(p.extension());
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e == ".ttf" || e == ".otf" || e == ".ttc" || e == ".otc";
}
} // namespace

namespace {
std::string normalisedPath(const std::string& p) {
    std::string n = p;
    for (char& c : n) {
        if (c == '\\') c = '/';
#if defined(_WIN32) || defined(__APPLE__)
        c = char(std::tolower(static_cast<unsigned char>(c)));
#endif
    }
    return n;
}
} // namespace

std::size_t FontCatalog::addFile(const std::string& path) {
    const std::string key = normalisedPath(path);
    if (std::find(m_paths.begin(), m_paths.end(), key) != m_paths.end()) return 0;
    m_paths.push_back(key);
    std::ifstream f(toPath(path), std::ios::binary);
    if (!f) return 0;
    std::vector<std::uint8_t> head, dir, name, os2, hd;
    if (!readAt(f, 0, 12, head)) return 0;
    std::vector<std::uint32_t> offsets;
    if (std::memcmp(head.data(), "ttcf", 4) == 0) {
        const std::uint32_t n = std::min<std::uint32_t>(be32(head.data() + 8), 256);
        std::vector<std::uint8_t> offs;
        if (!readAt(f, 12, std::size_t(n) * 4, offs)) return 0;
        for (std::uint32_t i = 0; i < n; ++i) offsets.push_back(be32(offs.data() + i * 4));
    } else {
        offsets.push_back(0);
    }
    std::size_t added = 0;
    for (std::uint32_t idx = 0; idx < offsets.size(); ++idx) {
        if (!readAt(f, offsets[idx], 12, head)) continue;
        const std::uint32_t tag = be32(head.data());
        if (tag != 0x00010000u && tag != 0x4F54544Fu /* OTTO */ && tag != 0x74727565u /* true */) continue;
        const std::uint16_t numTables = be16(head.data() + 4);
        if (numTables == 0 || !readAt(f, std::uint64_t(offsets[idx]) + 12, std::size_t(numTables) * 16, dir)) continue;
        std::uint32_t nameOff = 0, nameLen = 0, os2Off = 0, os2Len = 0, headOff = 0, headLen = 0;
        bool cmap = false, outlines = false;
        for (std::uint32_t t = 0; t < numTables; ++t) {
            const std::uint8_t* e = dir.data() + t * 16;
            const std::uint32_t off = be32(e + 8), len = be32(e + 12);
            if (std::memcmp(e, "name", 4) == 0) { nameOff = off; nameLen = len; }
            else if (std::memcmp(e, "OS/2", 4) == 0) { os2Off = off; os2Len = len; }
            else if (std::memcmp(e, "head", 4) == 0) { headOff = off; headLen = len; }
            else if (std::memcmp(e, "cmap", 4) == 0) cmap = true;
            else if (std::memcmp(e, "glyf", 4) == 0 || std::memcmp(e, "CFF ", 4) == 0 || std::memcmp(e, "CFF2", 4) == 0)
                outlines = true;
        }
        if (!cmap || !outlines || !nameOff || nameLen > (1u << 20) || !readAt(f, nameOff, nameLen, name)) continue;
        FontFaceInfo info;
        info.path = path;
        info.index = idx;
        info.family = nameString(name, 16);
        if (info.family.empty()) info.family = nameString(name, 1);
        info.style = nameString(name, 17);
        if (info.style.empty()) info.style = nameString(name, 2);
        info.fullName = nameString(name, 4);
        if (info.family.empty()) continue;
        if (os2Off && os2Len >= 64 && readAt(f, os2Off, std::min<std::uint32_t>(os2Len, 96), os2)) {
            info.weight = be16(os2.data() + 4);
            info.stretch = be16(os2.data() + 6);
            for (int k = 0; k < 4; ++k) info.unicodeRanges[k] = be32(os2.data() + 42 + k * 4);
            const std::uint16_t fsSel = be16(os2.data() + 62);
            info.italic = (fsSel & 0x0001u) || (fsSel & 0x0200u);
        }
        if (headOff && headLen >= 46 && readAt(f, headOff, 46, hd)) info.italic |= (be16(hd.data() + 44) & 0x0002u) != 0;
        if (info.weight == 0) info.weight = 400;
        if (info.weight < 10) info.weight = std::uint16_t(info.weight * 100); // some old fonts store 1..9
        if (info.stretch == 0 || info.stretch > 9) info.stretch = 5;
        m_faces.push_back(std::move(info));
        ++added;
    }
    return added;
}

std::size_t FontCatalog::scanDirectory(const std::string& dir) {
    std::size_t n = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(toPath(dir), fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || !hasFontExtension(it->path())) continue;
        n += addFile(fromPath(it->path()));
    }
    return n;
}

std::vector<std::string> FontCatalog::systemFontDirectories() {
    std::vector<std::string> dirs;
    auto env = [](const char* name) -> std::string {
#if defined(_WIN32)
        wchar_t buf[1024];
        std::wstring wname(name, name + std::strlen(name));
        const DWORD n = GetEnvironmentVariableW(wname.c_str(), buf, 1024);
        if (n == 0 || n >= 1024) return {};
        return fromPath(fs::path(std::wstring(buf, n)));
#else
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
#endif
    };
#if defined(_WIN32)
    std::string win = env("WINDIR");
    if (win.empty()) win = "C:\\Windows";
    dirs.push_back(win + "\\Fonts");
    if (const std::string local = env("LOCALAPPDATA"); !local.empty()) dirs.push_back(local + "\\Microsoft\\Windows\\Fonts");
#elif defined(__APPLE__)
    dirs = {"/System/Library/Fonts", "/Library/Fonts"};
    if (const std::string home = env("HOME"); !home.empty()) dirs.push_back(home + "/Library/Fonts");
#else
    dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
    const std::string home = env("HOME");
    if (const std::string xdg = env("XDG_DATA_HOME"); !xdg.empty()) dirs.push_back(xdg + "/fonts");
    else if (!home.empty()) dirs.push_back(home + "/.local/share/fonts");
    if (!home.empty()) dirs.push_back(home + "/.fonts");
#endif
    return dirs;
}

namespace {
#if defined(_WIN32)
// Font files listed in the registry (HKLM: machine-wide, file names relative to %WINDIR%\Fonts; HKCU: per-user, full
// paths under %LOCALAPPDATA%).
void registryFonts(HKEY root, const std::string& fontDir, std::vector<std::string>& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        wchar_t data[1024];
        DWORD nameLen = 512, dataLen = sizeof data, type = 0;
        const LONG r = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataLen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        std::wstring file(data, dataLen / sizeof(wchar_t));
        while (!file.empty() && file.back() == L'\0') file.pop_back();
        if (file.empty()) continue;
        fs::path p(file);
        if (p.is_relative()) p = toPath(fontDir) / p;
        std::error_code ec;
        if (fs::is_regular_file(p, ec) && hasFontExtension(p)) out.push_back(fromPath(p));
    }
    RegCloseKey(key);
}
#endif
} // namespace

const FontCatalog& FontCatalog::system() {
    static const FontCatalog catalog = [] {
        FontCatalog c;
        const std::vector<std::string> dirs = systemFontDirectories();
#if defined(_WIN32)
        std::vector<std::string> files;
        registryFonts(HKEY_LOCAL_MACHINE, dirs[0], files);
        registryFonts(HKEY_CURRENT_USER, dirs[0], files);
        for (const std::string& f : files) c.addFile(f);
#endif
        for (const std::string& d : dirs) c.scanDirectory(d);
        return c;
    }();
    return catalog;
}

std::vector<std::string> FontCatalog::families() const {
    std::vector<std::string> out;
    for (const FontFaceInfo& f : m_faces) out.push_back(f.family);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    });
    out.erase(std::unique(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return equalCi(a, b); }),
              out.end());
    return out;
}

namespace {
// Platform families for the generic CSS names, Windows / macOS / Linux mixed: the first installed one wins.
constexpr const char* kSans[] = {"Segoe UI", "Helvetica Neue", "Noto Sans", "DejaVu Sans", "Liberation Sans", "Arial", "Ubuntu", "Cantarell"};
constexpr const char* kSerif[] = {"Times New Roman", "Noto Serif", "DejaVu Serif", "Liberation Serif", "Times", "Georgia"};
constexpr const char* kMono[] = {"Cascadia Mono", "Consolas", "SF Mono", "Menlo", "DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "Courier New"};

std::span<const char* const> preferredFor(Script s) {
    static constexpr const char* kLatin[] = {"Segoe UI", "Helvetica Neue", "Noto Sans", "DejaVu Sans", "Arial", "Liberation Sans"};
    static constexpr const char* kArabic[] = {"Segoe UI", "Noto Sans Arabic", "Noto Naskh Arabic", "Geeza Pro", "Arial", "Tahoma", "DejaVu Sans"};
    static constexpr const char* kHebrew[] = {"Segoe UI", "Noto Sans Hebrew", "Arial Hebrew", "Arial", "Tahoma", "DejaVu Sans"};
    static constexpr const char* kDeva[] = {"Nirmala UI", "Noto Sans Devanagari", "Kohinoor Devanagari", "Mangal", "Lohit Devanagari"};
    static constexpr const char* kThai[] = {"Leelawadee UI", "Noto Sans Thai", "Thonburi", "Tahoma", "Loma"};
    static constexpr const char* kHan[] = {"Microsoft YaHei UI", "Microsoft YaHei", "PingFang SC", "Noto Sans CJK SC", "Noto Sans SC",
                                           "Source Han Sans SC", "WenQuanYi Micro Hei", "Yu Gothic UI", "SimSun", "Droid Sans Fallback"};
    static constexpr const char* kKana[] = {"Yu Gothic UI", "Yu Gothic", "Meiryo", "Hiragino Sans", "Noto Sans CJK JP", "Noto Sans JP",
                                            "MS Gothic", "Microsoft YaHei UI", "Droid Sans Fallback"};
    static constexpr const char* kHangul[] = {"Malgun Gothic", "Apple SD Gothic Neo", "Noto Sans CJK KR", "Noto Sans KR", "NanumGothic", "Gulim"};
    static constexpr const char* kEmoji[] = {"Segoe UI Emoji", "Apple Color Emoji", "Noto Color Emoji", "Twemoji Mozilla", "Twemoji"};
    static constexpr const char* kSym[] = {"Segoe UI Symbol", "Noto Sans Symbols 2", "Noto Sans Symbols2", "Noto Sans Symbols", "Apple Symbols", "DejaVu Sans"};
    switch (s) {
    case Script::Common:
    case Script::Latin:
    case Script::Greek:
    case Script::Cyrillic: return kLatin;
    case Script::Arabic: return kArabic;
    case Script::Hebrew: return kHebrew;
    case Script::Devanagari: return kDeva;
    case Script::Thai: return kThai;
    case Script::Han: return kHan;
    case Script::Kana: return kKana;
    case Script::Hangul: return kHangul;
    case Script::Emoji: return kEmoji;
    case Script::Symbols: return kSym;
    }
    return kLatin;
}

const FontFaceInfo* bestOfFamily(std::span<const FontFaceInfo> faces, std::string_view family, std::uint16_t weight,
                                 bool italic) {
    const FontFaceInfo* best = nullptr;
    int bestCost = 0;
    for (const FontFaceInfo& f : faces) {
        if (!equalCi(f.family, family)) continue;
        // CSS font matching order: stretch, then style, then weight.
        const int cost = std::abs(int(f.stretch) - 5) * 100000 + (f.italic != italic ? 10000 : 0) +
                         std::abs(int(f.weight) - int(weight)) * 2 + (int(f.weight) > int(weight) && weight <= 400 ? 1 : 0);
        if (!best || cost < bestCost) {
            best = &f;
            bestCost = cost;
        }
    }
    return best;
}
} // namespace

const FontFaceInfo* FontCatalog::match(std::string_view family, std::uint16_t weight, bool italic) const {
    if (const FontFaceInfo* f = bestOfFamily(m_faces, family, weight, italic)) return f;
    std::span<const char* const> generic;
    if (equalCi(family, "sans-serif") || equalCi(family, "sans serif") || equalCi(family, "system-ui") ||
        equalCi(family, "system ui"))
        generic = kSans;
    else if (equalCi(family, "serif")) generic = kSerif;
    else if (equalCi(family, "monospace")) generic = kMono;
    for (const char* g : generic)
        if (const FontFaceInfo* f = bestOfFamily(m_faces, g, weight, italic)) return f;
    return nullptr;
}

namespace {
// A letter every font that really supports the script maps (0: no check).
char32_t probeOf(Script s) {
    switch (s) {
    case Script::Common:
    case Script::Latin: return U'A';
    case Script::Greek: return U'Ω';
    case Script::Cyrillic: return U'Ж';
    case Script::Hebrew: return U'א';
    case Script::Arabic: return U'ب';
    case Script::Devanagari: return U'क';
    case Script::Thai: return U'ก';
    case Script::Han: return U'中';
    case Script::Kana: return U'あ';
    case Script::Hangul: return U'한';
    case Script::Emoji:
    case Script::Symbols: return 0;
    }
    return 0;
}

// Whether the face's cmap maps `cp` (format 4 or 12 Unicode subtable). Reads the table directory and cmap only.
bool cmapMaps(const FontFaceInfo& face, char32_t cp) {
    std::ifstream f(toPath(face.path), std::ios::binary);
    std::vector<std::uint8_t> head, dir, cmap;
    if (!f || !readAt(f, 0, 12, head)) return false;
    std::uint64_t base = 0;
    if (std::memcmp(head.data(), "ttcf", 4) == 0) {
        std::vector<std::uint8_t> off;
        if (!readAt(f, 12 + std::uint64_t(face.index) * 4, 4, off)) return false;
        base = be32(off.data());
        if (!readAt(f, base, 12, head)) return false;
    }
    const std::uint16_t numTables = be16(head.data() + 4);
    if (!readAt(f, base + 12, std::size_t(numTables) * 16, dir)) return false;
    for (std::uint32_t t = 0; t < numTables; ++t) {
        const std::uint8_t* e = dir.data() + t * 16;
        if (std::memcmp(e, "cmap", 4) != 0) continue;
        const std::uint32_t len = be32(e + 12);
        if (len < 4 || len > (64u << 20) || !readAt(f, be32(e + 8), len, cmap)) return false;
        break;
    }
    if (cmap.size() < 4) return false;
    // best subtable: full-repertoire format 12 first, then BMP format 4
    std::uint32_t best = 0;
    int bestScore = 0;
    const std::uint16_t n = be16(cmap.data() + 2);
    for (std::uint32_t i = 0; i < n && 4 + i * 8 + 8 <= cmap.size(); ++i) {
        const std::uint8_t* r = cmap.data() + 4 + i * 8;
        const std::uint16_t plat = be16(r), enc = be16(r + 2);
        const std::uint32_t off = be32(r + 4);
        if (off + 2 > cmap.size() || !(plat == 0 || (plat == 3 && (enc == 1 || enc == 10)))) continue;
        const std::uint16_t fmt = be16(cmap.data() + off);
        const int score = fmt == 12 ? 2 : fmt == 4 ? 1 : 0;
        if (score > bestScore) { bestScore = score; best = off; }
    }
    if (!bestScore) return false;
    const std::uint8_t* s = cmap.data() + best;
    const std::size_t avail = cmap.size() - best;
    if (bestScore == 2) {
        if (avail < 16) return false;
        const std::uint32_t groups = be32(s + 12);
        for (std::uint32_t g = 0; g < groups && 16 + std::size_t(g) * 12 + 12 <= avail; ++g) {
            const std::uint8_t* p = s + 16 + std::size_t(g) * 12;
            if (cp >= be32(p) && cp <= be32(p + 4)) return be32(p + 8) + (cp - be32(p)) != 0;
        }
        return false;
    }
    if (cp > 0xFFFF || avail < 14) return false;
    const std::size_t seg = be16(s + 6) / 2;
    if (16 + seg * 8 > avail) return false;
    const std::uint8_t* ends = s + 14;
    const std::uint8_t* starts = ends + seg * 2 + 2;
    const std::uint8_t* deltas = starts + seg * 2;
    const std::uint8_t* ranges = deltas + seg * 2;
    for (std::size_t i = 0; i < seg; ++i) {
        if (cp > be16(ends + i * 2)) continue;
        const std::uint16_t start = be16(starts + i * 2);
        if (cp < start) return false;
        const std::uint16_t ro = be16(ranges + i * 2), delta = be16(deltas + i * 2);
        if (!ro) return std::uint16_t(cp + delta) != 0;
        const std::size_t at = std::size_t(ranges + i * 2 - s) + ro + (cp - start) * 2;
        if (at + 2 > avail) return false;
        const std::uint16_t glyph = be16(s + at);
        return glyph && std::uint16_t(glyph + delta) != 0;
    }
    return false;
}
} // namespace

std::vector<const FontFaceInfo*> FontCatalog::systemFallbackChain(Script script, std::size_t maxFaces) const {
    std::vector<const FontFaceInfo*> out;
    const char32_t probe = probeOf(script);
    auto addFamily = [&](std::string_view fam) {
        if (out.size() >= maxFaces) return;
        for (const FontFaceInfo* f : out)
            if (equalCi(f->family, fam)) return;
        // OS/2 range bits are only hints (DejaVu claims Thai / CJK blocks it has no letters for): check the cmap too
        if (const FontFaceInfo* f = bestOfFamily(m_faces, fam, 400, false); f && f->covers(script) && (!probe || cmapMaps(*f, probe)))
            out.push_back(f);
    };
    for (const char* fam : preferredFor(script)) addFamily(fam);
    for (const FontFaceInfo& f : m_faces)
        if (f.covers(script) && f.stretch == 5) addFamily(f.family);
    return out;
}

std::expected<Font, TextError> FontCatalog::load(const FontFaceInfo& face) { return Font::fromFile(face.path, face.index); }

} // namespace fuse::text
