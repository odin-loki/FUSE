#include "fuse/ui/extras/i18n.hpp"

#include "fuse/ui/os.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fuse::ui::i18n {
namespace {
std::string lang2(std::string_view l) {
    std::string s(l.substr(0, l.find_first_of("_-.@")));
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
void putUtf8(std::string& o, char32_t c) {
    if (c < 0x80) o += char(c);
    else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
    else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
}
} // namespace

LayoutDirection directionOf(std::string_view language) {
    static constexpr std::string_view rtl[] = {"ar", "he", "iw", "fa", "ur", "yi", "ps", "sd", "ug", "dv", "ckb", "syr"};
    const std::string l = lang2(language);
    for (auto r : rtl) if (l == r) return LayoutDirection::RightToLeft;
    return LayoutDirection::LeftToRight;
}

int pluralCount(std::string_view language) {
    const std::string l = lang2(language);
    if (l == "ja" || l == "zh" || l == "ko" || l == "th" || l == "vi" || l == "id" || l == "ms" || l == "tr") return 1;
    if (l == "ru" || l == "uk" || l == "be" || l == "sr" || l == "hr" || l == "bs" || l == "pl" || l == "cs" || l == "sk" || l == "lt") return 3;
    if (l == "ar") return 6;
    if (l == "ga") return 5;
    if (l == "sl") return 4;
    return 2;
}

int pluralIndex(std::string_view language, long long n) {
    const std::string l = lang2(language);
    const long long a = n < 0 ? -n : n, m10 = a % 10, m100 = a % 100;
    switch (pluralCount(l)) {
    case 1: return 0;
    case 3:
        if (l == "pl") return a == 1 ? 0 : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? 1 : 2;
        if (l == "cs" || l == "sk") return a == 1 ? 0 : (a >= 2 && a <= 4) ? 1 : 2;
        if (l == "lt") return (m10 == 1 && m100 != 11) ? 0 : (m10 >= 2 && (m100 < 10 || m100 >= 20)) ? 1 : 2;
        return (m10 == 1 && m100 != 11) ? 0 : (m10 >= 2 && m10 <= 4 && (m100 < 10 || m100 >= 20)) ? 1 : 2;
    case 4: return m100 == 1 ? 0 : m100 == 2 ? 1 : (m100 == 3 || m100 == 4) ? 2 : 3;
    case 5: return a == 1 ? 0 : a == 2 ? 1 : (a >= 3 && a <= 6) ? 2 : (a >= 7 && a <= 10) ? 3 : 4;
    case 6: return a == 0 ? 0 : a == 1 ? 1 : a == 2 ? 2 : (m100 >= 3 && m100 <= 10) ? 3 : m100 >= 11 ? 4 : 5;
    default:
        if (l == "fr" || l == "pt_br" || l == "hy") return a > 1 ? 1 : 0;
        return a == 1 ? 0 : 1;
    }
}

Locale Locale::fromName(std::string_view name) {
    Locale L;
    L.name = std::string(name.substr(0, name.find_first_of(".@")));
    if (L.name.empty() || L.name == "C" || L.name == "POSIX") L.name = "en_US";
    const std::string l = lang2(L.name);
    L.direction = directionOf(l);
    if (l == "de" || l == "nl" || l == "it" || l == "es" || l == "pt" || l == "id" || l == "tr" || l == "da" || l == "el") { L.decimal = U','; L.group = U'.'; }
    else if (l == "fr" || l == "ru" || l == "uk" || l == "pl" || l == "cs" || l == "sv" || l == "fi" || l == "nb" || l == "sk") { L.decimal = U','; L.group = U'\u202F'; }
    else if (l == "ar" || l == "fa") { L.decimal = U'\u066B'; L.group = U'\u066C'; }
    else if (l == "hi") { L.groupSize = 2; } // lakh grouping after the first 3
    if (L.name == "de_CH") { L.decimal = U'.'; L.group = U'\u2019'; }
    return L;
}
Locale Locale::system() {
    for (const char* v : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const std::string s = os::env(v);
        if (!s.empty()) return fromName(s);
    }
    return fromName("en_US");
}
std::string Locale::formatInteger(long long v) const {
    std::string digits = std::to_string(v < 0 ? -(unsigned long long)v : (unsigned long long)v), o;
    if (v < 0) o += '-';
    const int n = int(digits.size());
    for (int i = 0; i < n; ++i) {
        o += digits[i];
        const int left = n - 1 - i;
        if (left <= 0) continue;
        const bool sep = groupSize == 2 ? (left == 3 || (left > 3 && (left - 3) % 2 == 0)) : left % groupSize == 0;
        if (sep) putUtf8(o, group);
    }
    return o;
}
std::string Locale::formatNumber(double v, int decimals) const {
    const double p = std::pow(10.0, decimals);
    const double r = std::round(std::abs(v) * p);
    const long long ip = (long long)(r / p);
    std::string o = (v < 0 && r != 0) ? "-" : "";
    o += formatInteger(ip);
    if (decimals > 0) {
        char b[32];
        std::snprintf(b, sizeof b, "%0*lld", decimals, (long long)(r - double(ip) * p));
        putUtf8(o, decimal);
        o += b;
    }
    return o;
}

std::string format(std::string_view pat, std::initializer_list<std::string_view> args) {
    std::string o;
    const auto* a = args.begin();
    for (std::size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] == '%' && i + 1 < pat.size()) {
            const char c = pat[i + 1];
            if (c == '%') { o += '%'; ++i; continue; }
            if (c >= '1' && c <= '9' && std::size_t(c - '1') < args.size()) { o += a[c - '1']; ++i; continue; }
        }
        o += pat[i];
    }
    return o;
}

// ---- .po ------------------------------------------------------------------------------------------------------------------
std::string Translator::key(std::string_view ctx, std::string_view src) {
    std::string k(ctx);
    k += '\x04';
    k += src;
    return k;
}
void Translator::insert(std::string_view c, std::string_view s, std::string_view t) { m_map[key(c, s)] = {std::string(t)}; }
void Translator::insertPlural(std::string_view c, std::string_view s, std::vector<std::string> f) { m_map[key(c, s)] = std::move(f); }

namespace {
bool unquote(std::string_view s, std::string& out) {
    const auto a = s.find('"'), b = s.rfind('"');
    if (a == std::string_view::npos || b <= a) return false;
    for (std::size_t i = a + 1; i < b; ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < b) {
            c = s[++i];
            switch (c) { case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; default: break; }
        }
        out += c;
    }
    return true;
}
} // namespace

bool Translator::loadPo(std::string_view po) {
    struct Entry { std::string ctx, id, plural; std::vector<std::string> str; bool fuzzy = false, any = false; } e;
    std::string* cur = nullptr;
    bool ok = true;
    auto commit = [&] {
        if (e.any) {
            if (e.id.empty()) { // header
                const std::string h = e.str.empty() ? std::string() : e.str[0];
                const auto p = h.find("Language:");
                if (p != std::string::npos) {
                    auto q = h.find_first_not_of(' ', p + 9);
                    auto r = h.find('\n', q);
                    if (q != std::string::npos) m_lang = h.substr(q, r - q);
                }
            } else if (!e.fuzzy && !e.str.empty() && !e.str[0].empty()) {
                m_map[key(e.ctx, e.id)] = e.str;
            }
        }
        e = {};
        cur = nullptr;
    };
    std::size_t pos = 0;
    while (pos <= po.size()) {
        auto nl = po.find('\n', pos);
        std::string_view line = po.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? po.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        const auto fs = line.find_first_not_of(' ');
        if (fs == std::string_view::npos) { if (e.any && !e.str.empty()) commit(); continue; }
        line = line.substr(fs);
        if (line.starts_with("#,")) { if (e.any && !e.str.empty()) commit(); if (line.find("fuzzy") != std::string_view::npos) e.fuzzy = true; continue; }
        if (line[0] == '#') continue;
        auto field = [&](std::string_view kw, std::string& dst) {
            if (!line.starts_with(kw) || (line.size() > kw.size() && line[kw.size()] != ' ')) return false;
            dst.clear();
            ok = unquote(line.substr(kw.size()), dst) && ok;
            cur = &dst;
            e.any = true;
            return true;
        };
        if (line.starts_with("msgctxt")) { if (!e.str.empty()) { bool f = e.fuzzy; commit(); e.fuzzy = f; } field("msgctxt", e.ctx); continue; }
        if (line.starts_with("msgid_plural")) { field("msgid_plural", e.plural); continue; }
        if (line.starts_with("msgid")) { if (!e.str.empty()) { bool f = e.fuzzy; commit(); e.fuzzy = f; } field("msgid", e.id); continue; }
        if (line.starts_with("msgstr[")) {
            const auto rb = line.find(']');
            if (rb == std::string_view::npos) { ok = false; continue; }
            const int idx = std::atoi(std::string(line.substr(7, rb - 7)).c_str());
            if (idx < 0 || idx > 16) { ok = false; continue; }
            if (int(e.str.size()) <= idx) e.str.resize(idx + 1);
            ok = unquote(line.substr(rb + 1), e.str[idx]) && ok;
            cur = &e.str[idx];
            continue;
        }
        if (line.starts_with("msgstr")) { e.str.assign(1, {}); ok = unquote(line.substr(6), e.str[0]) && ok; cur = &e.str[0]; continue; }
        if (line[0] == '"' && cur) { ok = unquote(line, *cur) && ok; continue; }
        ok = false;
    }
    commit();
    return ok;
}
bool Translator::loadPoFile(std::string_view path) {
    auto f = os::readFile(path);
    return f && loadPo(*f);
}
std::string_view Translator::tr(std::string_view c, std::string_view s) const {
    auto it = m_map.find(key(c, s));
    return it == m_map.end() || it->second.empty() ? s : std::string_view(it->second[0]);
}
std::string Translator::trn(std::string_view c, std::string_view sing, std::string_view plu, long long n) const {
    std::string r;
    auto it = m_map.find(key(c, sing));
    if (it != m_map.end() && !it->second.empty()) {
        const int idx = std::min<int>(pluralIndex(m_lang, n), int(it->second.size()) - 1);
        r = it->second[idx];
    } else {
        r = std::string(n == 1 ? sing : plu);
    }
    const std::string ns = std::to_string(n);
    for (std::size_t p; (p = r.find("%n")) != std::string::npos;) r.replace(p, 2, ns);
    return r;
}

namespace {
std::vector<const Translator*>& installed() { static std::vector<const Translator*> v; return v; }
} // namespace
void installTranslator(const Translator* t) { if (t) { removeTranslator(t); installed().push_back(t); } }
void removeTranslator(const Translator* t) { std::erase(installed(), t); }
std::string_view tr(std::string_view c, std::string_view s) {
    for (auto it = installed().rbegin(); it != installed().rend(); ++it) {
        std::string_view r = (*it)->tr(c, s);
        if (r.data() != s.data()) return r;
    }
    return s;
}
std::string trn(std::string_view c, std::string_view s, std::string_view p, long long n) {
    if (installed().empty()) { Translator t; return t.trn(c, s, p, n); }
    return installed().back()->trn(c, s, p, n);
}
LayoutDirection applicationDirection() {
    return installed().empty() ? LayoutDirection::LeftToRight : directionOf(installed().back()->language());
}

} // namespace fuse::ui::i18n
