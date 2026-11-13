// Script classification, per-script system fallback chains and fvar named-instance listing over the installed-font
// list of font_family.cpp (fuse/text/font_family.hpp). Reads only the cmap of the candidate faces.
#include "fuse/text/font_family.hpp"
#include "text_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace fuse::text {

namespace {
fs::path toPath(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}
} // namespace

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
bool rangeBit(const SystemFontInfo& f, int bit) { return (f.unicodeRanges[bit / 32] >> (bit % 32)) & 1u; }
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

bool SystemFontInfo::covers(Script s) const {
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

std::string nameString(const std::vector<std::uint8_t>& t, std::uint16_t nameId) { return detail::fontNameString(t, nameId); }

// CSS font-stretch percentage -> OS/2 usWidthClass.
std::uint16_t widthClass(float pct) {
    static constexpr float kPct[] = {50.f, 62.5f, 75.f, 87.5f, 100.f, 112.5f, 125.f, 150.f, 200.f};
    std::uint16_t best = 5;
    for (std::uint16_t k = 0; k < 9; ++k)
        if (std::abs(kPct[k] - pct) < std::abs(kPct[best - 1] - pct)) best = std::uint16_t(k + 1);
    return best;
}

bool styleKeyword(std::string_view w) {
    static constexpr std::string_view kWords[] = {
        "thin", "hairline", "extralight", "ultralight", "light", "semilight", "demilight", "regular", "normal", "book",
        "medium", "semibold", "demibold", "bold", "extrabold", "ultrabold", "black", "heavy", "condensed", "semicondensed",
        "extracondensed", "ultracondensed", "expanded", "semiexpanded", "extraexpanded", "wide", "narrow", "italic",
        "oblique"};
    for (const std::string_view k : kWords)
        if (equalCi(k, w)) return true;
    return false;
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    for (std::size_t i = 0; i < s.size();) {
        while (i < s.size() && s[i] == ' ') ++i;
        const std::size_t b = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

// One SystemFontInfo per fvar named instance of `base` (see SystemFontInfo::namedInstance).
std::size_t addNamedInstances(const SystemFontInfo base, const std::vector<std::uint8_t>& name,
                              const std::vector<std::uint8_t>& fv, const std::vector<std::uint8_t>& st,
                              std::vector<SystemFontInfo>& out) {
    const std::size_t n = fv.size();
    const std::uint32_t axesOff = be16(fv.data() + 4), axisCount = be16(fv.data() + 8), axisSize = be16(fv.data() + 10);
    const std::uint32_t instCount = be16(fv.data() + 12), instSize = be16(fv.data() + 14);
    if (!axisCount || axisSize < 20 || instSize < 4 + 4 * axisCount ||
        std::uint64_t(axesOff) + std::uint64_t(axisCount) * axisSize + std::uint64_t(instCount) * instSize > n)
        return 0;
    std::vector<std::uint32_t> tags;
    for (std::uint32_t a = 0; a < axisCount; ++a) tags.push_back(be32(fv.data() + axesOff + a * axisSize));
    // STAT design axes and axis values (formats 1-3): (axis tag, value range, flags, name)
    struct Value {
        std::uint32_t tag;
        float lo, hi;
        std::uint16_t flags;
        std::string name;
    };
    std::vector<std::uint32_t> statAxes;
    std::vector<Value> values;
    if (st.size() >= 18) {
        const std::uint32_t axisRec = be16(st.data() + 4), axes = be16(st.data() + 6), axesAt = be32(st.data() + 8);
        const std::uint32_t valueCount = be16(st.data() + 12), valuesAt = be32(st.data() + 14);
        if (axisRec >= 8 && std::uint64_t(axesAt) + std::uint64_t(axes) * axisRec <= st.size())
            for (std::uint32_t a = 0; a < axes; ++a) statAxes.push_back(be32(st.data() + axesAt + a * axisRec));
        for (std::uint32_t k = 0; k < valueCount && valuesAt && std::uint64_t(valuesAt) + k * 2 + 2 <= st.size(); ++k) {
            const std::uint64_t v = std::uint64_t(valuesAt) + be16(st.data() + valuesAt + k * 2);
            if (v + 12 > st.size()) continue;
            const std::uint16_t fmt = be16(st.data() + v), ai = be16(st.data() + v + 2);
            if (fmt < 1 || fmt > 3 || ai >= statAxes.size() || (fmt == 2 && v + 20 > st.size())) continue;
            const auto fixed = [&](std::uint64_t p) { return float(std::int32_t(be32(st.data() + p))) / 65536.f; };
            Value val{statAxes[ai], fixed(v + 8), fixed(v + 8), be16(st.data() + v + 4), nameString(name, be16(st.data() + v + 6))};
            if (fmt == 2) {
                val.lo = fixed(v + 12);
                val.hi = fixed(v + 16);
            }
            values.push_back(std::move(val));
        }
    }
    std::size_t added = 0;
    for (std::uint32_t k = 0; k < instCount; ++k) {
        const std::uint8_t* rec = fv.data() + axesOff + axisCount * axisSize + k * instSize;
        const std::string sub = nameString(name, be16(rec));
        if (sub.empty()) continue;
        SystemFontInfo info = base;
        info.namedInstance = int(k);
        std::vector<std::string> extra; // family words from non-WWS axes
        for (std::uint32_t a = 0; a < axisCount; ++a) {
            const float c = float(std::int32_t(be32(rec + 4 + a * 4))) / 65536.f;
            const std::uint32_t t = tags[a];
            if (t == fontTag("wght")) info.weight = int(std::clamp(std::lround(c), 1l, 1000l));
            else if (t == fontTag("wdth")) info.stretch = int(widthClass(c));
            else if (t == fontTag("ital")) info.italic = c >= 0.5f;
            else if (t == fontTag("slnt")) info.italic = c < 0.f;
            else if (!values.empty())
                for (const Value& v : values) {
                    const bool hit = v.lo == v.hi ? std::abs(c - v.lo) < 1e-3f : (c >= v.lo && c < v.hi);
                    if (v.tag == t && hit) {
                        if (!(v.flags & 2u) && !v.name.empty()) extra.push_back(v.name); // not elidable
                        break;
                    }
                }
        }
        std::string style;
        for (const std::string_view w : words(sub)) {
            bool family = false;
            if (values.empty()) family = !styleKeyword(w); // no STAT: non-style words of the subfamily name
            else
                for (const std::string& e : extra) family |= equalCi(e, w);
            if (family) {
                if (values.empty()) extra.emplace_back(w);
                continue;
            }
            if (!style.empty()) style += ' ';
            style += w;
        }
        for (const std::string& e : extra) info.family += ' ' + e;
        info.style = style.empty() ? "Regular" : style;
        info.fullName = info.family + (equalCi(info.style, "Regular") ? std::string() : ' ' + info.style);
        out.push_back(std::move(info));
        ++added;
    }
    return added;
}
} // namespace

std::size_t detail::catalogNamedInstances(const SystemFontInfo& base, const std::vector<std::uint8_t>& name,
                                          const std::vector<std::uint8_t>& fvar, const std::vector<std::uint8_t>& stat,
                                          std::vector<SystemFontInfo>& out) {
    return addNamedInstances(base, name, fvar, stat, out);
}
detail::NamedInstanceLister& detail::namedInstanceLister() {
    static NamedInstanceLister fn = nullptr;
    return fn;
}

namespace {
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

const SystemFontInfo* bestOfFamily(std::span<const SystemFontInfo> faces, std::string_view family, int weight, bool italic) {
    const SystemFontInfo* best = nullptr;
    int bestCost = 0;
    for (const SystemFontInfo& f : faces) {
        if (!equalCi(f.family, family)) continue;
        // CSS font matching order: stretch, then style, then weight.
        const int cost = std::abs(f.stretch - 5) * 100000 + (f.italic != italic ? 10000 : 0) +
                         std::abs(f.weight - weight) * 4 + (f.weight > weight && weight <= 400 ? 2 : 0) +
                         (f.namedInstance >= 0 ? 1 : 0);
        if (!best || cost < bestCost) {
            best = &f;
            bestCost = cost;
        }
    }
    return best;
}
} // namespace

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
bool cmapMaps(const SystemFontInfo& face, char32_t cp) {
    std::ifstream f(toPath(face.path), std::ios::binary);
    std::vector<std::uint8_t> head, dir, cmap;
    if (!f || !readAt(f, 0, 12, head)) return false;
    std::uint64_t base = 0;
    if (std::memcmp(head.data(), "ttcf", 4) == 0) {
        std::vector<std::uint8_t> off;
        if (!readAt(f, 12 + std::uint64_t(face.faceIndex) * 4, 4, off)) return false;
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

std::vector<const SystemFontInfo*> systemFallbackChain(Script script, std::size_t maxFaces) {
    std::vector<const SystemFontInfo*> out;
    const std::span<const SystemFontInfo> m_faces = systemFonts();
    const char32_t probe = probeOf(script);
    auto addFamily = [&](std::string_view fam) {
        if (out.size() >= maxFaces) return;
        for (const SystemFontInfo* f : out)
            if (equalCi(f->family, fam)) return;
        // OS/2 range bits are only hints (DejaVu claims Thai / CJK blocks it has no letters for): check the cmap too
        if (const SystemFontInfo* f = bestOfFamily(m_faces, fam, 400, false); f && f->covers(script) && (!probe || cmapMaps(*f, probe)))
            out.push_back(f);
    };
    for (const char* fam : preferredFor(script)) addFamily(fam);
    for (const SystemFontInfo& f : m_faces)
        if (f.covers(script) && f.stretch == 5) addFamily(f.family);
    return out;
}

} // namespace fuse::text
