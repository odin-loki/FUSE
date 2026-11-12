// fuse_text — dependency-free i18n helpers (FUSE UI plan §4.8; Qt: QLocale, QTranslator plural forms).
//
// Built-in tables only (no ICU, no CLDR data files): cardinal plural rules for the common CLDR rule families, the
// grouping / decimal separators of a table of common locales, and a pseudo-localisation transform for layout testing.
// Locales are BCP 47 / POSIX tags ("en", "en-GB", "pt_BR", "ru-RU"); an unknown region falls back to its language and
// an unknown language to English rules. Digits are always Western (0-9): Arabic-Indic and other native digit sets
// are not produced.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fuse::text {

// ---- Plurals -------------------------------------------------------------------------------------------------------
enum class PluralCategory : std::uint8_t { Zero, One, Two, Few, Many, Other };
/// "zero", "one", "two", "few", "many", "other" (the CLDR keywords).
std::string_view pluralCategoryName(PluralCategory c);

/// CLDR cardinal plural category of the integer `n` in `locale`. Rule families: no plural (ja, zh, ko, vi, th, id,
/// ms, ...): other; Germanic / Romance style (en, de, nl, sv, it, es, el, hu, tr, ...): one, other; French style
/// (fr, pt, hi, bn, fa, ...: 0 and 1 are "one"); East Slavic (ru, uk, be), Polish, Czech / Slovak, South Slavic
/// (sr, hr, bs), Slovenian, Lithuanian, Latvian, Romanian; Arabic (zero, one, two, few, many, other), Hebrew, Welsh,
/// Irish.
PluralCategory pluralCategory(std::string_view locale, long long n);
/// Same for a decimal shown with `fractionDigits` digits after the separator (CLDR operands n, i, v, f): "1.0" is
/// "other" in English, "1,5" is "other" in Russian, "0,5" is "one" in French.
PluralCategory pluralCategory(std::string_view locale, double value, int fractionDigits);

/// Picks the branch of an ICU-style plural pattern for `c`: "one {# file} other {# files}" (also "=0 {no files}"
/// exact matches through selectPlural(pattern, locale, n)). Falls back to the "other" branch, then to the whole
/// pattern when it has no branches.
std::string_view selectPlural(std::string_view pattern, PluralCategory c);
std::string_view selectPlural(std::string_view pattern, std::string_view locale, long long n);
/// Appends the chosen branch to `out` with every '#' replaced by `n` formatted for the locale.
void formatPlural(std::string& out, std::string_view pattern, std::string_view locale, long long n);
std::string formatPlural(std::string_view pattern, std::string_view locale, long long n);

// ---- Numbers -------------------------------------------------------------------------------------------------------
struct NumberSymbols {
    std::string_view decimal = ".";
    std::string_view group = ",";
    std::uint8_t primaryGroup = 3;   ///< digits in the group next to the decimal separator
    std::uint8_t secondaryGroup = 3; ///< digits in the other groups (2 for the Indian system: 12,34,567)
    std::uint8_t minGroupingDigits = 1; ///< 2: no separator in four-digit numbers (es, pl, pt-PT: "1234", "12 345")
};
/// Separators of `locale` from the built-in table (en, de, fr, es, it, pt, nl, sv, nb, da, fi, pl, cs, sk, ru, uk,
/// bg, hu, ro, el, tr, ja, zh, ko, hi, ar, he, id, vi, th, de-CH, en-IN, ...); English for unknown locales.
const NumberSymbols& numberSymbols(std::string_view locale);

/// Appends `value` with grouping and exactly `fractionDigits` decimals (rounded half away from zero), e.g.
/// de: 1234567.891, 2 -> "1.234.567,89". `grouping` false omits the group separators.
void formatNumber(std::string& out, std::string_view locale, double value, int fractionDigits = 0, bool grouping = true);
void formatInteger(std::string& out, std::string_view locale, long long value, bool grouping = true);
std::string formatNumber(std::string_view locale, double value, int fractionDigits = 0, bool grouping = true);
std::string formatInteger(std::string_view locale, long long value, bool grouping = true);

// ---- Pseudo-localisation -------------------------------------------------------------------------------------------
struct PseudoOptions {
    float expansion = 0.3f; ///< extra length as a fraction of the letter count (translations run longer than English)
    bool brackets = true;   ///< wrap in [ ] so a clipped or concatenated string is visible
    bool accents = true;    ///< replace ASCII letters by accented look-alikes (exposes hard-coded strings, missing glyphs)
};
/// Pseudo-localises a UI string: "Save file" -> "[Šávé ƒíļé ···]" style. Placeholders and markup are kept verbatim:
/// "{name}", "{0}", "%s" / "%1$d" style printf conversions, "<b>" tags, "&amp;" entities and the plural marker '#'.
std::string pseudoLocalize(std::string_view s, const PseudoOptions& opt = {});

} // namespace fuse::text
