// fuse/text/i18n.hpp's plural and number functions (namespace fuse::text), implemented on fuse_ui_l10n's tables: one
// set of CLDR plural rules (plural.cpp) and one number-symbol table (format.cpp) for both APIs. What stays here is the
// small "one {# file} other {# files}" pattern syntax and fixed-fraction formatting with half-away-from-zero rounding
// (the l10n formatNumber rounds half-even on the shortest decimal, as ICU does).
#include "fuse/text/i18n.hpp"

#include "fuse/ui/l10n/l10n.hpp"
#include "l10n_internal.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace fuse::text {

namespace l10n = fuse::ui::l10n;

namespace {
using ull = unsigned long long;
using PC = PluralCategory;

double pow10i(int d) {
    double p = 1.0;
    for (int k = 0; k < d; ++k) p *= 10.0;
    return p;
}

// PluralCategory and l10n::Plural list the CLDR keywords in the same order.
static_assert(int(PC::Zero) == int(l10n::Plural::Zero) && int(PC::Other) == int(l10n::Plural::Other));
PC cardinal(std::string_view locale, const l10n::PluralOperands& op) {
    // this API's documented fallback: a language without rules pluralises like English ("1 file", "2 files")
    return PC(l10n::pluralCardinal(l10n::hasPluralRules(locale) ? locale : std::string_view("en"), op));
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

} // namespace

std::string_view pluralCategoryName(PluralCategory c) { return l10n::pluralName(l10n::Plural(c)); }

PluralCategory pluralCategory(std::string_view locale, long long n) { return cardinal(locale, l10n::PluralOperands::fromInt(n)); }

PluralCategory pluralCategory(std::string_view locale, double value, int fractionDigits) {
    fractionDigits = fractionDigits < 0 ? 0 : (fractionDigits > 9 ? 9 : fractionDigits);
    const double scale = pow10i(fractionDigits);
    double a = std::fabs(value);
    if (!(a < 9.0e15 / scale)) a = 0.0; // out of range (or NaN): no meaningful category
    const ull scaled = ull(std::llround(a * scale));
    // the operands of the number as it is shown: i, and f with exactly `fractionDigits` digits (v)
    l10n::PluralOperands op;
    op.i = scaled / ull(scale);
    op.v = std::uint32_t(fractionDigits);
    op.f = scaled % ull(scale);
    op.t = op.f;
    while (op.t && op.t % 10 == 0) op.t /= 10;
    op.n = double(scaled) / scale;
    return cardinal(locale, op);
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
    // views of the l10n table's rows (static storage there), built once
    static const std::vector<NumberSymbols> rows = [] {
        std::vector<NumberSymbols> r(l10n::detail::numberRowCount());
        for (std::size_t k = 0; k < r.size(); ++k) {
            const l10n::detail::NumberRow n = l10n::detail::numberRow(k);
            r[k] = {n.decimal, n.group, std::uint8_t(n.primary), std::uint8_t(n.secondary), std::uint8_t(n.minGrouping)};
        }
        return r;
    }();
    return rows[l10n::detail::numberRowIndex(locale)];
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

} // namespace fuse::text
