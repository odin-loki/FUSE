// i18n helpers: CLDR plural rule families, number separators, pseudo-localisation (see fuse/text/i18n.hpp).
#include "fuse/text/i18n.hpp"

#include "fuse/text/text.hpp"

#include <cmath>
#include <cstdio>

namespace fuse::text {

namespace {
using ull = unsigned long long;

struct LocaleTag {
    char lang[4] = {0, 0, 0, 0};   // lower case, up to 3 letters
    char region[4] = {0, 0, 0, 0}; // upper case, up to 3 characters
};
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
char upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }
LocaleTag parseLocale(std::string_view s) {
    LocaleTag t;
    std::size_t i = 0;
    for (; i < s.size() && s[i] != '-' && s[i] != '_' && s[i] != '.' && s[i] != '@'; ++i)
        if (i < 3) t.lang[i] = lower(s[i]);
    // subtags: a 2-letter or 3-digit one is the region ("zh-Hant-TW", "sr_Latn_RS", "es-419")
    while (i < s.size() && (s[i] == '-' || s[i] == '_')) {
        const std::size_t b = ++i;
        while (i < s.size() && s[i] != '-' && s[i] != '_' && s[i] != '.' && s[i] != '@') ++i;
        if (i - b == 2 || i - b == 3) {
            for (std::size_t k = 0; k < i - b; ++k) t.region[k] = upper(s[b + k]);
            if (i - b == 2) break;
        }
    }
    return t;
}
bool is(const LocaleTag& t, std::string_view lang) { return lang == t.lang; }
template <std::size_t N> bool oneOf(const LocaleTag& t, const std::string_view (&list)[N]) {
    for (std::string_view l : list)
        if (l == t.lang) return true;
    return false;
}

/// CLDR plural operands: n (absolute value), i (integer digits), v (visible fraction digit count), f (fraction digits).
struct Operands {
    double n = 0;
    ull i = 0;
    int v = 0;
    ull f = 0;
};
using PC = PluralCategory;

constexpr std::string_view kNoPlural[] = {"ja", "zh", "ko", "vi", "th", "id", "ms", "km", "lo", "my", "yue",
                                          "jv", "su", "ig", "yo", "wo", "to", "bo", "dz", "in"};
constexpr std::string_view kNEqualsOne[] = {"bg", "el", "hu", "tr", "nb", "nn", "no", "da", "eu", "ka", "kk", "mn",
                                            "az", "uz", "sq", "af", "eo", "fo", "ta", "te", "ml", "mr", "ne", "or"};
constexpr std::string_view kZeroOneIsOne[] = {"fr", "hy", "kab", "ff"};
constexpr std::string_view kIZeroOrNOne[] = {"hi", "bn", "gu", "kn", "fa", "am", "zu", "as"};

PC rule(const LocaleTag& t, const Operands& o) {
    const ull i = o.i, i10 = i % 10, i100 = i % 100;
    const bool integer = o.v == 0;
    if (oneOf(t, kNoPlural)) return PC::Other;
    if (is(t, "ru") || is(t, "uk") || is(t, "be")) {
        if (!integer) return PC::Other;
        if (i10 == 1 && i100 != 11) return PC::One;
        if (i10 >= 2 && i10 <= 4 && !(i100 >= 12 && i100 <= 14)) return PC::Few;
        return PC::Many;
    }
    if (is(t, "pl")) {
        if (!integer) return PC::Other;
        if (i == 1) return PC::One;
        if (i10 >= 2 && i10 <= 4 && !(i100 >= 12 && i100 <= 14)) return PC::Few;
        return PC::Many;
    }
    if (is(t, "cs") || is(t, "sk")) {
        if (!integer) return PC::Many;
        if (i == 1) return PC::One;
        if (i >= 2 && i <= 4) return PC::Few;
        return PC::Other;
    }
    if (is(t, "sr") || is(t, "hr") || is(t, "bs") || is(t, "sh")) {
        const ull f10 = o.f % 10, f100 = o.f % 100;
        if ((integer && i10 == 1 && i100 != 11) || (!integer && f10 == 1 && f100 != 11)) return PC::One;
        if ((integer && i10 >= 2 && i10 <= 4 && !(i100 >= 12 && i100 <= 14)) ||
            (!integer && f10 >= 2 && f10 <= 4 && !(f100 >= 12 && f100 <= 14)))
            return PC::Few;
        return PC::Other;
    }
    if (is(t, "sl")) {
        if (!integer) return PC::Few;
        if (i100 == 1) return PC::One;
        if (i100 == 2) return PC::Two;
        if (i100 == 3 || i100 == 4) return PC::Few;
        return PC::Other;
    }
    if (is(t, "lt")) {
        if (o.f != 0) return PC::Many;
        const bool teens = i100 >= 11 && i100 <= 19;
        if (i10 == 1 && !teens) return PC::One;
        if (i10 >= 2 && i10 <= 9 && !teens) return PC::Few;
        return PC::Other;
    }
    if (is(t, "lv")) {
        if (integer || o.f == 0) {
            if (i10 == 0 || (i100 >= 11 && i100 <= 19)) return PC::Zero;
            if (i10 == 1 && i100 != 11) return PC::One;
            return PC::Other;
        }
        const ull f10 = o.f % 10, f100 = o.f % 100;
        if (o.v == 2 && f100 >= 11 && f100 <= 19) return PC::Zero;
        if (f10 == 1 && !(o.v == 2 && f100 == 11)) return PC::One;
        return PC::Other;
    }
    if (is(t, "ro") || is(t, "mo")) {
        if (integer && i == 1) return PC::One;
        if (!integer || i == 0 || (i != 1 && i100 >= 1 && i100 <= 19)) return PC::Few;
        return PC::Other;
    }
    if (is(t, "ar")) {
        if (!integer && o.f != 0) return PC::Other;
        if (i == 0) return PC::Zero;
        if (i == 1) return PC::One;
        if (i == 2) return PC::Two;
        if (i100 >= 3 && i100 <= 10) return PC::Few;
        if (i100 >= 11) return PC::Many;
        return PC::Other;
    }
    if (is(t, "he") || is(t, "iw")) {
        if (integer && i == 1) return PC::One;
        if (integer && i == 2) return PC::Two;
        return PC::Other;
    }
    if (is(t, "cy")) {
        if (!integer && o.f != 0) return PC::Other;
        switch (i) {
        case 0: return PC::Zero;
        case 1: return PC::One;
        case 2: return PC::Two;
        case 3: return PC::Few;
        case 6: return PC::Many;
        default: return PC::Other;
        }
    }
    if (is(t, "ga")) {
        if (!integer && o.f != 0) return PC::Other;
        if (i == 1) return PC::One;
        if (i == 2) return PC::Two;
        if (i >= 3 && i <= 6) return PC::Few;
        if (i >= 7 && i <= 10) return PC::Many;
        return PC::Other;
    }
    if (oneOf(t, kZeroOneIsOne)) return (i == 0 || i == 1) ? PC::One : PC::Other;
    if (is(t, "pt")) {
        // Brazilian Portuguese counts 0 and 1 (and 1,5) as "one"; European Portuguese only the integer 1
        if (std::string_view(t.region) == "PT") return (integer && i == 1) ? PC::One : PC::Other;
        return (i == 0 || i == 1) ? PC::One : PC::Other;
    }
    if (oneOf(t, kIZeroOrNOne)) return (i == 0 || o.n == 1.0) ? PC::One : PC::Other;
    if (oneOf(t, kNEqualsOne)) return o.n == 1.0 ? PC::One : PC::Other;
    return (integer && i == 1) ? PC::One : PC::Other; // en, de, nl, sv, it, es, ca, fi, et, ... and unknown languages
}

double pow10i(int d) {
    double p = 1.0;
    for (int k = 0; k < d; ++k) p *= 10.0;
    return p;
}

/// Next "keyword {text}" branch of a plural pattern from `i`; false at the end. Braces nest.
bool nextBranch(std::string_view p, std::size_t& i, std::string_view& key, std::string_view& body) {
    while (i < p.size() && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) ++i;
    const std::size_t kb = i;
    while (i < p.size() && p[i] != '{' && p[i] != ' ' && p[i] != '\t' && p[i] != '\n') ++i;
    key = p.substr(kb, i - kb);
    while (i < p.size() && p[i] != '{') {
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && p[i] != '\r') return false;
        ++i;
    }
    if (i >= p.size() || key.empty()) return false;
    const std::size_t bb = ++i;
    int depth = 1;
    while (i < p.size() && depth > 0) {
        if (p[i] == '{') ++depth;
        else if (p[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return false;
    body = p.substr(bb, i - 1 - bb);
    return true;
}

void appendGrouped(std::string& out, const char* digits, std::size_t n, const NumberSymbols& sym, bool grouping) {
    if (!grouping || n < std::size_t(sym.primaryGroup) + sym.minGroupingDigits) {
        out.append(digits, n);
        return;
    }
    // separators sit `primaryGroup` digits from the right, then every `secondaryGroup`
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t right = n - k; // digits from here to the end
        if (k > 0 && right >= sym.primaryGroup && (right - sym.primaryGroup) % sym.secondaryGroup == 0) out += sym.group;
        out.push_back(digits[k]);
    }
}

struct SymbolRow {
    std::string_view locale; // "de" or "de-CH"
    NumberSymbols sym;
};
constexpr std::string_view kNbsp = "\xC2\xA0", kNnbsp = "\xE2\x80\xAF", kApos = "\xE2\x80\x99";
constexpr NumberSymbols kEnglish{};
// clang-format off
constexpr SymbolRow kSymbols[] = {
    {"en", {".", ",", 3, 3, 1}},    {"en-IN", {".", ",", 3, 2, 1}}, {"hi", {".", ",", 3, 2, 1}},
    {"bn", {".", ",", 3, 2, 1}},    {"en-ZA", {",", kNbsp, 3, 3, 1}},
    {"ja", {".", ",", 3, 3, 1}},    {"zh", {".", ",", 3, 3, 1}},    {"ko", {".", ",", 3, 3, 1}},
    {"he", {".", ",", 3, 3, 1}},    {"th", {".", ",", 3, 3, 1}},    {"ar", {".", ",", 3, 3, 1}},
    {"de", {",", ".", 3, 3, 1}},    {"de-CH", {".", kApos, 3, 3, 1}}, {"de-LI", {".", kApos, 3, 3, 1}},
    {"de-AT", {",", kNbsp, 3, 3, 1}},
    {"nl", {",", ".", 3, 3, 1}},    {"it", {",", ".", 3, 3, 1}},    {"it-CH", {".", kApos, 3, 3, 1}},
    {"es", {",", ".", 3, 3, 2}},    {"es-MX", {".", ",", 3, 3, 1}}, {"es-US", {".", ",", 3, 3, 1}},
    {"pt", {",", ".", 3, 3, 1}},    {"pt-PT", {",", kNbsp, 3, 3, 2}},
    {"ca", {",", ".", 3, 3, 2}},    {"gl", {",", ".", 3, 3, 1}},
    {"id", {",", ".", 3, 3, 1}},    {"tr", {",", ".", 3, 3, 1}},    {"da", {",", ".", 3, 3, 1}},
    {"el", {",", ".", 3, 3, 1}},    {"hr", {",", ".", 3, 3, 2}},    {"sl", {",", ".", 3, 3, 1}},
    {"sr", {",", ".", 3, 3, 1}},    {"ro", {",", ".", 3, 3, 1}},    {"vi", {",", ".", 3, 3, 1}},
    {"fr", {",", kNnbsp, 3, 3, 1}}, {"fr-CH", {",", kNnbsp, 3, 3, 1}}, {"fr-CA", {",", kNbsp, 3, 3, 1}},
    {"nb", {",", kNbsp, 3, 3, 1}},  {"nn", {",", kNbsp, 3, 3, 1}},  {"no", {",", kNbsp, 3, 3, 1}},
    {"sv", {",", kNbsp, 3, 3, 1}},  {"fi", {",", kNbsp, 3, 3, 1}},  {"cs", {",", kNbsp, 3, 3, 1}},
    {"sk", {",", kNbsp, 3, 3, 1}},  {"ru", {",", kNbsp, 3, 3, 1}},  {"uk", {",", kNbsp, 3, 3, 1}},
    {"be", {",", kNbsp, 3, 3, 1}},  {"pl", {",", kNbsp, 3, 3, 2}},  {"bg", {",", kNbsp, 3, 3, 2}},
    {"hu", {",", kNbsp, 3, 3, 1}},  {"lt", {",", kNbsp, 3, 3, 1}},  {"lv", {",", kNbsp, 3, 3, 1}},
    {"et", {",", kNbsp, 3, 3, 2}},  {"kk", {",", kNbsp, 3, 3, 1}},
};
// clang-format on
} // namespace

std::string_view pluralCategoryName(PluralCategory c) {
    switch (c) {
    case PC::Zero: return "zero";
    case PC::One: return "one";
    case PC::Two: return "two";
    case PC::Few: return "few";
    case PC::Many: return "many";
    case PC::Other: return "other";
    }
    return "other";
}

PluralCategory pluralCategory(std::string_view locale, long long n) {
    Operands o;
    o.i = n < 0 ? ull(-(n + 1)) + 1ull : ull(n);
    o.n = double(o.i);
    return rule(parseLocale(locale), o);
}

PluralCategory pluralCategory(std::string_view locale, double value, int fractionDigits) {
    fractionDigits = fractionDigits < 0 ? 0 : (fractionDigits > 9 ? 9 : fractionDigits);
    const double scale = pow10i(fractionDigits);
    double a = std::fabs(value);
    if (!(a < 9.0e15 / scale)) a = 0.0; // out of range (or NaN): no meaningful category
    const ull scaled = ull(std::llround(a * scale));
    Operands o;
    o.i = scaled / ull(scale);
    o.v = fractionDigits;
    o.f = scaled % ull(scale);
    o.n = double(scaled) / scale;
    return rule(parseLocale(locale), o);
}

std::string_view selectPlural(std::string_view pattern, PluralCategory c) {
    const std::string_view want = pluralCategoryName(c);
    std::string_view other;
    bool any = false, haveOther = false;
    std::size_t i = 0;
    std::string_view key, body;
    while (nextBranch(pattern, i, key, body)) {
        any = true;
        if (key == want) return body;
        if (key == "other") {
            other = body;
            haveOther = true;
        }
    }
    if (haveOther) return other;
    return any ? std::string_view() : pattern;
}

std::string_view selectPlural(std::string_view pattern, std::string_view locale, long long n) {
    // exact "=N" branches win over the category
    std::size_t i = 0;
    std::string_view key, body;
    while (nextBranch(pattern, i, key, body)) {
        if (key.size() < 2 || key[0] != '=') continue;
        long long v = 0;
        bool ok = true, neg = false;
        std::size_t k = 1;
        if (key[k] == '-') { neg = true; ++k; }
        for (; k < key.size(); ++k) {
            if (key[k] < '0' || key[k] > '9') { ok = false; break; }
            v = v * 10 + (key[k] - '0');
        }
        if (ok && (neg ? -v : v) == n) return body;
    }
    return selectPlural(pattern, pluralCategory(locale, n));
}

void formatPlural(std::string& out, std::string_view pattern, std::string_view locale, long long n) {
    const std::string_view body = selectPlural(pattern, locale, n);
    for (char c : body) {
        if (c == '#') formatInteger(out, locale, n);
        else out.push_back(c);
    }
}
std::string formatPlural(std::string_view pattern, std::string_view locale, long long n) {
    std::string s;
    formatPlural(s, pattern, locale, n);
    return s;
}

const NumberSymbols& numberSymbols(std::string_view locale) {
    const LocaleTag t = parseLocale(locale);
    const std::string_view lang(t.lang), region(t.region);
    const NumberSymbols* byLang = nullptr;
    for (const SymbolRow& r : kSymbols) {
        const auto dash = r.locale.find('-');
        if (r.locale.substr(0, dash) != lang) continue;
        if (dash == std::string_view::npos) byLang = &r.sym;
        else if (!region.empty() && r.locale.substr(dash + 1) == region) return r.sym;
    }
    return byLang ? *byLang : kEnglish;
}

void formatInteger(std::string& out, std::string_view locale, long long value, bool grouping) {
    char buf[24];
    ull a = value < 0 ? ull(-(value + 1)) + 1ull : ull(value);
    std::size_t n = 0;
    do {
        buf[sizeof buf - 1 - n++] = char('0' + a % 10);
        a /= 10;
    } while (a);
    if (value < 0) out.push_back('-');
    appendGrouped(out, buf + sizeof buf - n, n, numberSymbols(locale), grouping);
}

void formatNumber(std::string& out, std::string_view locale, double value, int fractionDigits, bool grouping) {
    fractionDigits = fractionDigits < 0 ? 0 : (fractionDigits > 15 ? 15 : fractionDigits);
    const NumberSymbols& sym = numberSymbols(locale);
    if (std::isnan(value)) { out += "NaN"; return; }
    const bool neg = std::signbit(value);
    double a = std::fabs(value);
    if (std::isinf(a)) { out += neg ? "-\xE2\x88\x9E" : "\xE2\x88\x9E"; return; }
    const double scale = pow10i(fractionDigits);
    char digits[352];
    std::size_t n = 0;
    ull frac = 0;
    if (a * scale < 9.0e18) {
        const ull scaled = ull(std::llround(a * scale)); // half away from zero
        const ull ip = scaled / ull(scale);
        frac = scaled % ull(scale);
        ull v = ip;
        char tmp[24];
        do {
            tmp[n++] = char('0' + v % 10);
            v /= 10;
        } while (v);
        for (std::size_t k = 0; k < n; ++k) digits[k] = tmp[n - 1 - k];
    } else { // beyond 64-bit: integer digits from printf, fraction zero
        const int w = std::snprintf(digits, sizeof digits, "%.0f", a);
        n = w > 0 ? std::size_t(w) : 0;
    }
    bool zero = frac == 0;
    for (std::size_t k = 0; k < n && zero; ++k) zero = digits[k] == '0';
    if (neg && !zero) out.push_back('-'); // no "-0,00"
    appendGrouped(out, digits, n, sym, grouping);
    if (fractionDigits > 0) {
        out += sym.decimal;
        char f[16];
        for (int k = fractionDigits - 1; k >= 0; --k) {
            f[k] = char('0' + frac % 10);
            frac /= 10;
        }
        out.append(f, std::size_t(fractionDigits));
    }
}
std::string formatNumber(std::string_view locale, double value, int fractionDigits, bool grouping) {
    std::string s;
    formatNumber(s, locale, value, fractionDigits, grouping);
    return s;
}
std::string formatInteger(std::string_view locale, long long value, bool grouping) {
    std::string s;
    formatInteger(s, locale, value, grouping);
    return s;
}

// ---- Pseudo-localisation -------------------------------------------------------------------------------------------
namespace {
// Accented look-alikes from Latin-1 / Latin Extended-A (covered by the bundled UI font), indexed by letter.
constexpr char32_t kPseudoLower[26] = {0x00E1, 'b',    0x00E7, 0x00F0, 0x00E9, 0x0192, 0x011D, 0x0125, 0x00ED,
                                       0x0135, 0x0137, 0x013C, 'm',    0x00F1, 0x00F6, 'p',    'q',    0x0155,
                                       0x0161, 0x0163, 0x00FC, 'v',    0x0175, 'x',    0x00FD, 0x017E};
constexpr char32_t kPseudoUpper[26] = {0x00C5, 'B',    0x00C7, 0x00D0, 0x00C9, 'F',    0x011C, 0x0124, 0x00CE,
                                       0x0134, 0x0136, 0x013B, 'M',    0x00D1, 0x00D6, 'P',    'Q',    0x0154,
                                       0x0160, 0x0162, 0x00DB, 'V',    0x0174, 'X',    0x00DD, 0x017D};
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
/// Length of the verbatim span starting at `i` (placeholder, printf conversion, tag, entity); 0 when none.
std::size_t protectedSpan(std::string_view s, std::size_t i) {
    const char c = s[i];
    if (c == '{') {
        int depth = 0;
        for (std::size_t k = i; k < s.size(); ++k) {
            if (s[k] == '{') ++depth;
            else if (s[k] == '}' && --depth == 0) return k - i + 1;
        }
        return 0;
    }
    if (c == '<') {
        const std::size_t e = s.find('>', i);
        if (e != std::string_view::npos && e > i + 1 && (isAlpha(s[i + 1]) || s[i + 1] == '/')) return e - i + 1;
        return 0;
    }
    if (c == '&') {
        std::size_t k = i + 1;
        while (k < s.size() && (isAlpha(s[k]) || isDigit(s[k]) || s[k] == '#') && k - i < 10) ++k;
        return (k < s.size() && s[k] == ';' && k > i + 1) ? k - i + 1 : 0;
    }
    if (c == '%' && i + 1 < s.size()) {
        if (s[i + 1] == '%') return 2;
        std::size_t k = i + 1;
        while (k < s.size() && (isDigit(s[k]) || s[k] == '$' || s[k] == '.' || s[k] == '-' || s[k] == '+' || s[k] == 'l' ||
                                s[k] == 'h' || s[k] == 'z'))
            ++k;
        if (k < s.size() && std::string_view("diuoxXfFeEgGscp@").find(s[k]) != std::string_view::npos) return k - i + 1;
        // Qt style %1 .. %99
        if (k > i + 1 && isDigit(s[k - 1])) return k - i;
        return 0;
    }
    return 0;
}
} // namespace

std::string pseudoLocalize(std::string_view s, const PseudoOptions& opt) {
    std::string out;
    out.reserve(s.size() * 2 + 8);
    if (opt.brackets) out.push_back('[');
    std::size_t letters = 0;
    for (std::size_t i = 0; i < s.size();) {
        if (const std::size_t span = protectedSpan(s, i)) {
            out.append(s.substr(i, span));
            i += span;
            continue;
        }
        const char c = s[i++];
        if (isAlpha(c)) {
            ++letters;
            if (opt.accents) {
                appendUtf8(out, (c >= 'a') ? kPseudoLower[c - 'a'] : kPseudoUpper[c - 'A']);
                continue;
            }
        }
        out.push_back(c);
    }
    if (opt.expansion > 0.f && letters > 0) {
        const std::size_t pad = std::size_t(std::ceil(float(letters) * opt.expansion));
        out.push_back(' ');
        for (std::size_t k = 0; k < pad; ++k) out += "\xC2\xB7"; // middle dots: visibly padding, never a word
    }
    if (opt.brackets) out.push_back(']');
    return out;
}

} // namespace fuse::text
