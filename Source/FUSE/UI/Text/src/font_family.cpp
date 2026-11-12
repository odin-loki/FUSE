// Font families, synthetic style selection and installed-font discovery (see fuse/text/font_family.hpp).
#include "fuse/text/font_family.hpp"

#include "sfnt_info.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace fuse::text {

// ---- sfnt tables ---------------------------------------------------------------------------------------------------
namespace detail {
namespace {
std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
std::uint32_t rd32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
constexpr std::uint32_t tag(char a, char b, char c, char d) {
    return (std::uint32_t(std::uint8_t(a)) << 24) | (std::uint32_t(std::uint8_t(b)) << 16) | (std::uint32_t(std::uint8_t(c)) << 8) |
           std::uint32_t(std::uint8_t(d));
}
bool isSfntVersion(std::uint32_t v) { return v == 0x00010000u || v == tag('O', 'T', 'T', 'O') || v == tag('t', 'r', 'u', 'e'); }

/// Offset of face `index` in the file; false when out of range or not a font.
bool faceOffset(const SfntReader& r, int index, std::uint32_t& off) {
    std::uint8_t h[12];
    if (index < 0 || !r.read(r.user, 0, h, 12)) return false;
    if (rd32(h) == tag('t', 't', 'c', 'f')) {
        const std::uint32_t n = rd32(h + 8);
        std::uint8_t o[4];
        if (std::uint32_t(index) >= n || !r.read(r.user, 12 + std::uint64_t(index) * 4, o, 4)) return false;
        off = rd32(o);
        std::uint8_t v[4];
        return r.read(r.user, off, v, 4) && isSfntVersion(rd32(v));
    }
    off = 0;
    return index == 0 && isSfntVersion(rd32(h));
}

struct Table {
    std::uint32_t offset = 0, length = 0;
};
void appendUtf8Cp(std::string& out, char32_t cp) { appendUtf8(out, cp); }

/// Decodes a name record: UTF-16BE (Windows, Unicode platforms) or Mac Roman (ASCII kept, the rest dropped).
std::string decodeName(const std::vector<std::uint8_t>& raw, bool utf16) {
    std::string s;
    if (!utf16) {
        for (std::uint8_t c : raw)
            if (c >= 0x20 && c < 0x7F) s.push_back(char(c));
        return s;
    }
    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
        char32_t c = rd16(raw.data() + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < raw.size()) {
            const char32_t lo = rd16(raw.data() + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (c >= 0x20) appendUtf8Cp(s, c);
    }
    return s;
}
} // namespace

int sfntFaceCount(const SfntReader& r) {
    std::uint8_t h[12];
    if (!r.read || !r.read(r.user, 0, h, 12)) return 0;
    if (rd32(h) == tag('t', 't', 'c', 'f')) {
        const std::uint32_t n = rd32(h + 8);
        return n > 4096 ? 0 : int(n);
    }
    return isSfntVersion(rd32(h)) ? 1 : 0;
}

bool sfntFaceInfo(const SfntReader& r, int index, FaceInfo& out) {
    std::uint32_t base = 0;
    if (!r.read || !faceOffset(r, index, base)) return false;
    std::uint8_t h[12];
    if (!r.read(r.user, base, h, 12)) return false;
    const std::uint16_t numTables = rd16(h + 4);
    if (numTables == 0 || numTables > 512) return false;
    std::vector<std::uint8_t> dir(std::size_t(numTables) * 16);
    if (!r.read(r.user, std::uint64_t(base) + 12, dir.data(), dir.size())) return false;
    Table name, os2, head, post;
    bool glyf = false, cff = false;
    for (std::uint16_t i = 0; i < numTables; ++i) {
        const std::uint8_t* e = dir.data() + std::size_t(i) * 16;
        const std::uint32_t t = rd32(e);
        const Table tb{rd32(e + 8), rd32(e + 12)};
        if (t == tag('n', 'a', 'm', 'e')) name = tb;
        else if (t == tag('O', 'S', '/', '2')) os2 = tb;
        else if (t == tag('h', 'e', 'a', 'd')) head = tb;
        else if (t == tag('p', 'o', 's', 't')) post = tb;
        else if (t == tag('g', 'l', 'y', 'f')) glyf = tb.length > 0;
        else if (t == tag('C', 'F', 'F', ' ')) cff = tb.length > 0;
    }
    out = {};
    out.outlines = glyf || cff;

    bool macBold = false;
    if (head.length >= 54) {
        std::uint8_t b[2];
        if (r.read(r.user, std::uint64_t(head.offset) + 44, b, 2)) {
            const std::uint16_t macStyle = rd16(b);
            macBold = (macStyle & 1) != 0;
            out.italic = (macStyle & 2) != 0;
        }
    }
    out.weight = macBold ? 700 : 400;
    if (os2.length >= 64) {
        std::uint8_t b[64];
        if (r.read(r.user, os2.offset, b, 64)) {
            int w = rd16(b + 4);
            if (w >= 1 && w <= 9) w *= 100; // legacy fonts store 1..9
            if (w >= 1 && w <= 1000) out.weight = w;
            out.strikeSize = std::int16_t(rd16(b + 26));
            out.strikePosition = std::int16_t(rd16(b + 28));
            out.hasStrike = true;
            const std::uint16_t fsSelection = rd16(b + 62);
            if (fsSelection & 0x0001u) out.italic = true;
            if (fsSelection & 0x0200u) out.italic = true; // OBLIQUE
        }
    }
    if (post.length >= 16) {
        std::uint8_t b[16];
        if (r.read(r.user, post.offset, b, 16)) {
            out.underlinePosition = std::int16_t(rd16(b + 8));
            out.underlineThickness = std::int16_t(rd16(b + 10));
            out.monospace = rd32(b + 12) != 0;
            out.hasPost = true;
        }
    }

    // name: family (16 typographic, else 1) and style (17, else 2); prefer Windows English, then Unicode, then Mac.
    if (name.length >= 6) {
        std::uint8_t nh[6];
        if (r.read(r.user, name.offset, nh, 6)) {
            const std::uint16_t count = std::min<std::uint16_t>(rd16(nh + 2), 2048);
            const std::uint32_t strings = rd16(nh + 4);
            std::vector<std::uint8_t> recs(std::size_t(count) * 12);
            if (count && r.read(r.user, std::uint64_t(name.offset) + 6, recs.data(), recs.size())) {
                struct Pick {
                    int rank = 0;
                    std::uint16_t len = 0, off = 0;
                    bool utf16 = false;
                };
                Pick pick[4]; // ids 1, 2, 16, 17
                for (std::uint16_t i = 0; i < count; ++i) {
                    const std::uint8_t* e = recs.data() + std::size_t(i) * 12;
                    const std::uint16_t platform = rd16(e), enc = rd16(e + 2), lang = rd16(e + 4), id = rd16(e + 6);
                    const int slot = id == 1 ? 0 : id == 2 ? 1 : id == 16 ? 2 : id == 17 ? 3 : -1;
                    if (slot < 0) continue;
                    int rank = 0;
                    bool utf16 = true;
                    if (platform == 3 && (enc == 1 || enc == 10)) rank = lang == 0x0409 ? 5 : ((lang & 0x3FF) == 0x09 ? 4 : 2);
                    else if (platform == 0) rank = 3;
                    else if (platform == 1 && enc == 0) { rank = lang == 0 ? 1 : 0; utf16 = false; }
                    if (rank > pick[slot].rank) pick[slot] = {rank, rd16(e + 8), rd16(e + 10), utf16};
                }
                auto text = [&](const Pick& p) -> std::string {
                    if (p.rank == 0 || p.len == 0 || p.len > 1024) return {};
                    const std::uint64_t at = std::uint64_t(name.offset) + strings + p.off;
                    if (std::uint64_t(strings) + p.off + p.len > name.length) return {};
                    std::vector<std::uint8_t> raw(p.len);
                    if (!r.read(r.user, at, raw.data(), raw.size())) return {};
                    return decodeName(raw, p.utf16);
                };
                out.family = text(pick[2]);
                if (out.family.empty()) out.family = text(pick[0]);
                out.style = text(pick[3]);
                if (out.style.empty()) out.style = text(pick[1]);
            }
        }
    }
    if (out.style.empty()) {
        out.style = out.weight >= 600 ? (out.italic ? "Bold Italic" : "Bold") : (out.italic ? "Italic" : "Regular");
    }
    return true;
}
} // namespace detail

// ---- Matching ------------------------------------------------------------------------------------------------------
namespace {
char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
std::string_view trimName(std::string_view s) {
    auto drop = [](char c) { return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '\n' || c == '\r'; };
    while (!s.empty() && drop(s.front())) s.remove_prefix(1);
    while (!s.empty() && drop(s.back())) s.remove_suffix(1);
    return s;
}
bool equalNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}
int compareNoCase(std::string_view a, std::string_view b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char x = lowerAscii(a[i]), y = lowerAscii(b[i]);
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}
} // namespace

std::uint32_t familyHash(std::string_view name) {
    name = trimName(name);
    std::uint32_t h = 2166136261u;
    for (char c : name) h = (h ^ std::uint8_t(lowerAscii(c))) * 16777619u;
    h = (h ^ (h >> 24)) & 0xFFFFFFu;
    return h ? h : 1u;
}

int fontMatchScore(int want, bool wantItalic, int w, bool italic) {
    int s = wantItalic == italic ? 0 : 10000;
    if (w == want) return s;
    if (want >= 400 && want <= 500) {
        if (w > want && w <= 500) return s + (w - want);
        if (w < want) return s + 1000 + (want - w);
        return s + 2000 + (w - 500);
    }
    if (want < 400) return s + (w < want ? want - w : 1000 + (w - want));
    return s + (w > want ? w - want : 1000 + (want - w));
}

// ---- FontFamily ----------------------------------------------------------------------------------------------------
const Font* FontFamily::add(Font face) {
    const int w = face.weight();
    const bool it = face.italic();
    return add(std::move(face), w, it);
}
const Font* FontFamily::add(Font face, int weight, bool italic) {
    if (!face.valid()) return nullptr;
    Member m;
    m.owned = std::make_unique<Font>(std::move(face));
    if (m_hasFallbacks) m.owned->setFallbacks(m_fallbacks);
    m.font = m.owned.get();
    m.weight = weight;
    m.italic = italic;
    if (m_name.empty()) m_name = std::string(m.font->familyName());
    m_members.push_back(std::move(m));
    return m_members.back().font;
}
void FontFamily::addExternal(const Font* face) {
    if (face && face->valid()) addExternal(face, face->weight(), face->italic());
}
void FontFamily::addExternal(const Font* face, int weight, bool italic) {
    if (!face || !face->valid()) return;
    Member m;
    m.font = face;
    m.weight = weight;
    m.italic = italic;
    if (m_name.empty()) m_name = std::string(face->familyName());
    m_members.push_back(std::move(m));
}

const FontFamily::Member* FontFamily::nearestMember(int weight, bool italic) const {
    const Member* best = nullptr;
    int bestScore = 0;
    for (const Member& m : m_members) {
        const int s = fontMatchScore(weight, italic, m.weight, m.italic);
        if (!best || s < bestScore) {
            best = &m;
            bestScore = s;
        }
    }
    return best;
}
bool FontFamily::hasReal(int weight, bool italic) const {
    for (const Member& m : m_members)
        if (m.weight == weight && m.italic == italic) return true;
    return false;
}
const Font* FontFamily::nearestReal(int weight, bool italic) const {
    const Member* m = nearestMember(weight, italic);
    return m ? m->font : nullptr;
}

FontFamily::Match FontFamily::match(int weight, bool italic) {
    Match r;
    weight = std::clamp(weight, 1, 1000);
    const Member* m = nearestMember(weight, italic);
    if (!m) return r;
    r.font = m->font;
    r.syntheticOblique = italic && !m->italic;
    const int lighter = weight - m->weight;
    r.syntheticBold = weight >= 600 && lighter >= 150;
    if (!r.syntheticBold && !r.syntheticOblique) return r;
    const int steps = r.syntheticBold ? std::min(12, (lighter + 25) / 50) : 0;
    for (const Variant& v : m_variants)
        if (v.base == m->font && v.boldSteps == steps && v.oblique == r.syntheticOblique) {
            r.font = v.font.get();
            return r;
        }
    Variant v;
    v.base = m->font;
    v.boldSteps = steps;
    v.oblique = r.syntheticOblique;
    v.font = std::make_unique<Font>(
        Font::synthesized(*m->font, kBoldEmbolden * float(steps) / 6.f, r.syntheticOblique ? kObliqueSlant : 0.f));
    if (!v.font->valid()) {
        r.syntheticBold = r.syntheticOblique = false;
        return r; // keep the real face
    }
    if (m_hasFallbacks && m->owned) v.font->setFallbacks(m_fallbacks);
    r.font = v.font.get();
    m_variants.push_back(std::move(v));
    return r;
}

void FontFamily::setFallbacks(std::vector<const Font*> chain) {
    m_fallbacks = std::move(chain);
    m_hasFallbacks = true;
    for (Member& m : m_members)
        if (m.owned) m.owned->setFallbacks(m_fallbacks);
    for (Variant& v : m_variants)
        for (const Member& m : m_members)
            if (m.font == v.base && m.owned) v.font->setFallbacks(m_fallbacks);
}

FontFamily FontFamily::bundled() {
    FontFamily fam;
    if (auto f = Font::bundled()) fam.add(std::move(*f));
    if (fam.m_name.empty()) fam.m_name = "Open Sans";
    return fam;
}

// ---- Installed fonts -----------------------------------------------------------------------------------------------
namespace {
bool readFile(void* user, std::uint64_t off, void* dst, std::size_t n) {
    std::FILE* fp = static_cast<std::FILE*>(user);
    if (off > 0x7FFFFFFFull) return false;
    if (std::fseek(fp, long(off), SEEK_SET) != 0) return false;
    return std::fread(dst, 1, n, fp) == n;
}
bool fontExtension(std::string_view path) {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view e = path.substr(dot + 1);
    return equalNoCase(e, "ttf") || equalNoCase(e, "otf") || equalNoCase(e, "ttc") || equalNoCase(e, "otc");
}
std::string envOr(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read once under the font list lock
    return v && *v ? std::string(v) : std::string(fallback);
}
void pushUnique(std::vector<std::string>& v, std::string s) {
    if (s.empty()) return;
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(std::move(s));
}
bool infoLess(const SystemFontInfo& a, const SystemFontInfo& b) {
    if (const int c = compareNoCase(a.family, b.family)) return c < 0;
    if (a.weight != b.weight) return a.weight < b.weight;
    return a.italic < b.italic;
}

struct SystemFontCache {
    std::mutex mutex;
    std::vector<std::string> dirs; // override; empty: platform directories
    // Every list ever built stays alive, so a span handed out before a refresh never dangles.
    std::vector<std::unique_ptr<std::vector<SystemFontInfo>>> lists;
    bool valid = false;
};
SystemFontCache& cache() {
    static SystemFontCache c;
    return c;
}
const std::vector<SystemFontInfo>& ensureScanned(SystemFontCache& c) { // lock held
    if (c.valid) return *c.lists.back();
    auto list = std::make_unique<std::vector<SystemFontInfo>>();
    const std::vector<std::string> dirs = c.dirs.empty() ? systemFontDirectories() : c.dirs;
    for (const std::string& d : dirs) scanFontDirectory(d, *list);
    // stable: for equal (family, weight, slant) the face from the earlier (more specific) directory wins
    std::stable_sort(list->begin(), list->end(), infoLess);
    c.lists.push_back(std::move(list));
    c.valid = true;
    return *c.lists.back();
}
template <class Pred> const SystemFontInfo* bestMatch(std::span<const SystemFontInfo> fonts, int weight, bool italic, Pred&& sameFamily) {
    const SystemFontInfo* best = nullptr;
    int bestScore = 0;
    for (const SystemFontInfo& f : fonts) {
        if (!sameFamily(f)) continue;
        const int s = fontMatchScore(weight, italic, f.weight, f.italic);
        if (!best || s < bestScore) {
            best = &f;
            bestScore = s;
        }
    }
    return best;
}
} // namespace

std::vector<std::string> systemFontDirectories() {
    std::vector<std::string> d;
#if defined(_WIN32)
    const std::string local = envOr("LOCALAPPDATA");
    if (!local.empty()) pushUnique(d, local + "\\Microsoft\\Windows\\Fonts");
    pushUnique(d, envOr("WINDIR", envOr("SystemRoot", "C:\\Windows").c_str()) + "\\Fonts");
#elif defined(__APPLE__)
    const std::string home = envOr("HOME");
    if (!home.empty()) pushUnique(d, home + "/Library/Fonts");
    pushUnique(d, "/Library/Fonts");
    pushUnique(d, "/System/Library/Fonts");
#elif defined(__ANDROID__)
    pushUnique(d, "/data/fonts");
    pushUnique(d, "/product/fonts");
    pushUnique(d, "/system/fonts");
#else
    const std::string home = envOr("HOME");
    const std::string dataHome = envOr("XDG_DATA_HOME", home.empty() ? "" : (home + "/.local/share").c_str());
    if (!dataHome.empty()) pushUnique(d, dataHome + "/fonts");
    if (!home.empty()) pushUnique(d, home + "/.fonts");
    const std::string dataDirs = envOr("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
    for (std::size_t i = 0; i <= dataDirs.size();) {
        const std::size_t e = std::min(dataDirs.find(':', i), dataDirs.size());
        std::string part = dataDirs.substr(i, e - i);
        while (part.size() > 1 && part.back() == '/') part.pop_back();
        if (!part.empty()) pushUnique(d, part + "/fonts");
        i = e + 1;
    }
    pushUnique(d, "/usr/local/share/fonts");
    pushUnique(d, "/usr/share/fonts");
    pushUnique(d, "/usr/local/lib/X11/fonts"); // FreeBSD ports (older layout)
    pushUnique(d, "/usr/X11R6/lib/X11/fonts"); // OpenBSD / NetBSD
    pushUnique(d, "/usr/pkg/share/fonts");     // NetBSD pkgsrc
#endif
    return d;
}

bool describeFontFile(const std::string& path, std::vector<SystemFontInfo>& out) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    const detail::SfntReader r{fp, readFile};
    const int faces = detail::sfntFaceCount(r);
    bool any = false;
    for (int i = 0; i < faces; ++i) {
        detail::FaceInfo fi;
        if (!detail::sfntFaceInfo(r, i, fi) || !fi.outlines || fi.family.empty() || fi.family[0] == '.') continue;
        SystemFontInfo s;
        s.family = std::move(fi.family);
        s.style = std::move(fi.style);
        s.path = path;
        s.faceIndex = i;
        s.weight = fi.weight;
        s.italic = fi.italic;
        s.monospace = fi.monospace;
        out.push_back(std::move(s));
        any = true;
    }
    std::fclose(fp);
    return any;
}

void scanFontDirectory(const std::string& dir, std::vector<SystemFontInfo>& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink, ec);
    if (ec) return;
    std::vector<std::string> files;
    const fs::recursive_directory_iterator end;
    for (std::size_t visited = 0; it != end && visited < 200000; it.increment(ec), ++visited) {
        if (ec) break;
        if (it.depth() > 8) { // a symlink loop or a tree that is not a font directory
            it.disable_recursion_pending();
            continue;
        }
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const auto u8 = it->path().u8string();
        std::string p(u8.begin(), u8.end());
        if (fontExtension(p)) files.push_back(std::move(p));
    }
    std::sort(files.begin(), files.end()); // directory order is not stable across file systems
    for (const std::string& f : files) describeFontFile(f, out);
}

std::span<const SystemFontInfo> systemFonts() {
    SystemFontCache& c = cache();
    const std::lock_guard lock(c.mutex);
    return ensureScanned(c);
}

std::vector<std::string> systemFontFamilies() {
    std::vector<std::string> out;
    for (const SystemFontInfo& f : systemFonts())
        if (out.empty() || !equalNoCase(out.back(), f.family)) out.push_back(f.family);
    return out;
}

const SystemFontInfo* findSystemFont(std::string_view family, int weight, bool italic) {
    family = trimName(family);
    return bestMatch(systemFonts(), weight, italic, [&](const SystemFontInfo& f) { return equalNoCase(f.family, family); });
}
const SystemFontInfo* findSystemFontByHash(std::uint32_t hash, int weight, bool italic) {
    return bestMatch(systemFonts(), weight, italic, [&](const SystemFontInfo& f) { return familyHash(f.family) == hash; });
}

void setSystemFontDirectories(std::vector<std::string> dirs) {
    SystemFontCache& c = cache();
    const std::lock_guard lock(c.mutex);
    c.dirs = std::move(dirs);
    c.valid = false;
}
void refreshSystemFonts() {
    SystemFontCache& c = cache();
    const std::lock_guard lock(c.mutex);
    c.valid = false;
}

std::expected<Font, TextError> Font::fromSystem(std::string_view family, int weight, bool italic) {
    const SystemFontInfo* f = findSystemFont(family, weight, italic);
    if (!f) return std::unexpected(TextError::InvalidFont);
    return fromFile(f->path, f->faceIndex);
}

std::expected<FontFamily, TextError> FontFamily::fromSystem(std::string_view family) {
    family = trimName(family);
    FontFamily fam;
    for (const SystemFontInfo& f : systemFonts()) {
        if (!equalNoCase(f.family, family)) continue;
        if (fam.hasReal(f.weight, f.italic)) continue; // same style installed twice: the first directory wins
        if (auto font = Font::fromFile(f.path, f.faceIndex)) {
            if (fam.m_name.empty()) fam.m_name = f.family;
            fam.add(std::move(*font), f.weight, f.italic);
        }
    }
    if (fam.empty()) return std::unexpected(TextError::InvalidFont);
    return fam;
}

} // namespace fuse::text
