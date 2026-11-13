// CLDR plural rules (CLDR 44 plurals.xml / ordinals.xml) for the locales fuse_ui_l10n ships, written out as code.
// The compact-exponent operands (c / e) are not supported: they are always 0.
#include "fuse/ui/l10n/l10n.hpp"

#include "l10n_internal.hpp"

#include "fuse/text/from_chars.hpp" // floating-point std::from_chars is missing from libc++ before LLVM 20 (NDK, FreeBSD, Apple)

#include <charconv>
#include <cmath>
#include <initializer_list>
#include <optional>

namespace fuse::ui::l10n {

namespace {
constexpr std::string_view kNames[] = {"zero", "one", "two", "few", "many", "other"};

bool in(std::uint64_t x, std::uint64_t lo, std::uint64_t hi) { return x >= lo && x <= hi; }

// "n = k" / "n % m = a..b": n must be an integer value (no non-zero fraction digits)
struct N {
    const PluralOperands& op;
    bool integral() const { return op.f == 0; }
    bool is(std::uint64_t k) const { return integral() && op.i == k; }
    bool mod(std::uint64_t m, std::uint64_t lo, std::uint64_t hi) const { return integral() && in(op.i % m, lo, hi); }
};
} // namespace

std::string_view pluralName(Plural p) { return kNames[std::size_t(p)]; }

bool pluralFromName(std::string_view name, Plural& out) {
    for (std::size_t k = 0; k < 6; ++k)
        if (kNames[k] == name) { out = Plural(k); return true; }
    return false;
}

bool PluralOperands::parse(std::string_view s, PluralOperands& out) {
    out = {};
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) s.remove_prefix(1);
    const std::size_t dot = s.find('.');
    const std::string_view ip = s.substr(0, dot), fp = dot == s.npos ? std::string_view{} : s.substr(dot + 1);
    if (ip.empty() || (dot != s.npos && fp.empty())) return false;
    for (char c : ip) {
        if (c < '0' || c > '9') return false;
        out.i = out.i > 1'000'000'000'000'000'000ull ? out.i : out.i * 10 + std::uint64_t(c - '0');
    }
    for (char c : fp) {
        if (c < '0' || c > '9') return false;
        if (out.v < 18) out.f = out.f * 10 + std::uint64_t(c - '0');
        ++out.v;
    }
    out.t = out.f;
    while (out.t && out.t % 10 == 0) out.t /= 10;
    double n = 0;
    fuse::text::fromChars(s.data(), s.data() + s.size(), n);
    out.n = n;
    return true;
}

PluralOperands PluralOperands::fromDouble(double x) {
    PluralOperands op;
    if (!std::isfinite(x)) return op;
    char buf[400];
    const auto r = detail::toCharsFixed(buf, buf + sizeof buf, std::fabs(x));
    if (r.ec != std::errc{}) return op;
    parse(std::string_view(buf, std::size_t(r.ptr - buf)), op);
    return op;
}

PluralOperands PluralOperands::fromInt(std::int64_t x) {
    PluralOperands op;
    op.i = x < 0 ? std::uint64_t(0) - std::uint64_t(x) : std::uint64_t(x);
    op.n = double(op.i);
    return op;
}

namespace {
// The cardinal rule of the tag's language; nullopt when the language has no rule here (root rule: other).
// This is the one plural-rule table of the UI: fuse/text/i18n.hpp (fuse::text::pluralCategory, text_i18n.cpp) and
// formatMessage both come here.
std::optional<Plural> cardinalRule(const detail::Tag& tag, const PluralOperands& op) {
    const std::string& l = tag.lang;
    const N n{op};
    const std::uint64_t i = op.i, v = op.v, f = op.f;
    auto is = [&](std::initializer_list<std::string_view> langs) {
        for (const std::string_view x : langs)
            if (x == l) return true;
        return false;
    };
    // one: i = 1 and v = 0
    if (is({"en", "de", "nl", "sv", "fi", "nb", "nn", "no", "et", "gl", "ca"})) return i == 1 && v == 0 ? Plural::One : Plural::Other;
    // da: one: n = 1 or t != 0 and i = 0,1
    if (l == "da") return n.is(1) || (op.t != 0 && i <= 1) ? Plural::One : Plural::Other;
    // one: n = 1
    if (is({"eu", "ka", "kk", "mn", "az", "uz", "sq", "af", "eo", "fo", "ta", "te", "ml", "mr", "ne", "or"}))
        return n.is(1) ? Plural::One : Plural::Other;
    // no plural forms
    if (is({"ja", "zh", "ko", "vi", "th", "id", "ms", "km", "lo", "my", "yue", "jv", "su", "ig", "yo", "wo", "to", "bo", "dz", "in"}))
        return Plural::Other;
    if (is({"sr", "hr", "bs", "sh"})) {
        const std::uint64_t f10 = f % 10, f100 = f % 100;
        if ((v == 0 && i % 10 == 1 && i % 100 != 11) || (v != 0 && f10 == 1 && f100 != 11)) return Plural::One;
        if ((v == 0 && in(i % 10, 2, 4) && !in(i % 100, 12, 14)) || (v != 0 && in(f10, 2, 4) && !in(f100, 12, 14)))
            return Plural::Few;
        return Plural::Other;
    }
    if (l == "sl") {
        if (v != 0) return Plural::Few;
        if (i % 100 == 1) return Plural::One;
        if (i % 100 == 2) return Plural::Two;
        if (in(i % 100, 3, 4)) return Plural::Few;
        return Plural::Other;
    }
    if (l == "lt") {
        if (f != 0) return Plural::Many;
        const bool teens = in(i % 100, 11, 19);
        if (i % 10 == 1 && !teens) return Plural::One;
        if (in(i % 10, 2, 9) && !teens) return Plural::Few;
        return Plural::Other;
    }
    if (l == "lv") {
        if (v == 0 || f == 0) {
            if (i % 10 == 0 || in(i % 100, 11, 19)) return Plural::Zero;
            if (i % 10 == 1 && i % 100 != 11) return Plural::One;
            return Plural::Other;
        }
        if (v == 2 && in(f % 100, 11, 19)) return Plural::Zero;
        if (f % 10 == 1 && !(v == 2 && f % 100 == 11)) return Plural::One;
        return Plural::Other;
    }
    if (l == "ro" || l == "mo") {
        if (i == 1 && v == 0) return Plural::One;
        if (v != 0 || i == 0 || (i != 1 && in(i % 100, 1, 19))) return Plural::Few;
        return Plural::Other;
    }
    if (l == "cy") {
        if (f != 0) return Plural::Other;
        switch (i) {
        case 0: return Plural::Zero;
        case 1: return Plural::One;
        case 2: return Plural::Two;
        case 3: return Plural::Few;
        case 6: return Plural::Many;
        default: return Plural::Other;
        }
    }
    if (l == "ga") {
        if (f != 0) return Plural::Other;
        if (i == 1) return Plural::One;
        if (i == 2) return Plural::Two;
        if (in(i, 3, 6)) return Plural::Few;
        if (in(i, 7, 10)) return Plural::Many;
        return Plural::Other;
    }
    // one: i = 0,1
    if (is({"hy", "kab", "ff"})) return i <= 1 ? Plural::One : Plural::Other;
    // one: i = 0 or n = 1
    if (is({"gu", "kn", "am", "zu", "as"})) return i == 0 || n.is(1) ? Plural::One : Plural::Other;
    // es: one n = 1; many (millions) e = 0 and i != 0 and i % 1000000 = 0 and v = 0
    const bool million = i != 0 && i % 1000000 == 0 && v == 0;
    if (l == "es") return n.is(1) ? Plural::One : million ? Plural::Many : Plural::Other;
    if (l == "it" || (l == "pt" && tag.region == "pt"))
        return i == 1 && v == 0 ? Plural::One : million ? Plural::Many : Plural::Other;
    if (l == "fr" || l == "pt") return i <= 1 ? Plural::One : million ? Plural::Many : Plural::Other;
    if (l == "ru" || l == "uk" || l == "be") {
        if (v != 0) return Plural::Other;
        if (i % 10 == 1 && i % 100 != 11) return Plural::One;
        if (in(i % 10, 2, 4) && !in(i % 100, 12, 14)) return Plural::Few;
        return Plural::Many;
    }
    if (l == "pl") {
        if (i == 1 && v == 0) return Plural::One;
        if (v != 0) return Plural::Other;
        if (in(i % 10, 2, 4) && !in(i % 100, 12, 14)) return Plural::Few;
        return Plural::Many;
    }
    if (l == "cs" || l == "sk") {
        if (i == 1 && v == 0) return Plural::One;
        if (in(i, 2, 4) && v == 0) return Plural::Few;
        return v != 0 ? Plural::Many : Plural::Other;
    }
    if (l == "ar") {
        if (n.is(0)) return Plural::Zero;
        if (n.is(1)) return Plural::One;
        if (n.is(2)) return Plural::Two;
        if (n.mod(100, 3, 10)) return Plural::Few;
        if (n.mod(100, 11, 99)) return Plural::Many;
        return Plural::Other;
    }
    if (l == "he" || l == "iw") {
        if ((i == 1 && v == 0) || (i == 0 && v != 0)) return Plural::One;
        if (i == 2 && v == 0) return Plural::Two;
        return Plural::Other;
    }
    if (l == "tr" || l == "el" || l == "hu" || l == "bg") return n.is(1) ? Plural::One : Plural::Other;
    if (l == "hi" || l == "bn" || l == "fa") return i == 0 || n.is(1) ? Plural::One : Plural::Other;
    return std::nullopt;
}
} // namespace

Plural pluralCardinal(std::string_view locale, const PluralOperands& op) {
    return cardinalRule(detail::splitTag(locale), op).value_or(Plural::Other); // unknown language: the root rule
}

bool hasPluralRules(std::string_view locale) { return cardinalRule(detail::splitTag(locale), PluralOperands{}).has_value(); }

Plural pluralOrdinal(std::string_view locale, const PluralOperands& op) {
    const detail::Tag tag = detail::splitTag(locale);
    const std::string& l = tag.lang;
    const N n{op};
    if (l == "en") {
        if (n.mod(10, 1, 1) && !n.mod(100, 11, 11)) return Plural::One;
        if (n.mod(10, 2, 2) && !n.mod(100, 12, 12)) return Plural::Two;
        if (n.mod(10, 3, 3) && !n.mod(100, 13, 13)) return Plural::Few;
        return Plural::Other;
    }
    if (l == "fr") return n.is(1) ? Plural::One : Plural::Other;
    if (l == "it") return n.is(11) || n.is(8) || n.is(80) || n.is(800) ? Plural::Many : Plural::Other;
    if (l == "sv") return n.mod(10, 1, 2) && !n.mod(100, 11, 12) ? Plural::One : Plural::Other;
    if (l == "uk") return n.mod(10, 3, 3) && !n.mod(100, 13, 13) ? Plural::Few : Plural::Other;
    if (l == "hi" || l == "bn") {
        if (n.is(1)) return Plural::One;
        if (n.is(2) || n.is(3)) return Plural::Two;
        if (n.is(4)) return Plural::Few;
        if (n.is(6)) return Plural::Many;
        return Plural::Other;
    }
    return Plural::Other; // de, es, pt, nl, ru, pl, cs, ar, he, ja, zh, ko, tr, fi, ...
}

} // namespace fuse::ui::l10n
