// fuse_text_i18n: the dependency-free i18n helpers of fuse_text (plan §4.8): CLDR cardinal plural categories for the
// common rule families (checked against the CLDR sample values), plural pattern selection / formatting, locale number
// formatting (grouping + decimal separators) and the pseudo-localisation transform; StringTable::trPlural when
// fuse_ui_game is built.
#include "test_util.hpp"

#include "fuse/text/i18n.hpp"

#if FUSE_UI_I18N_HAVE_GAME
#include "fuse/ui/game/game_ui.hpp"
#endif

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

using namespace fuse::ui::test;
using namespace fuse::text;
using PC = PluralCategory;

namespace {
void expect(const char* locale, PC want, std::initializer_list<long long> ns, int line) {
    for (long long n : ns)
        if (pluralCategory(locale, n) != want) {
            std::fprintf(stderr, "%s:%d: plural(%s, %lld) = %s, want %s\n", __FILE__, line, locale, n,
                         std::string(pluralCategoryName(pluralCategory(locale, n))).c_str(), std::string(pluralCategoryName(want)).c_str());
            ++failures();
        }
}
#define PLURAL(locale, want, ...) expect(locale, want, {__VA_ARGS__}, __LINE__)

void gatePlurals() {
    // one / other
    PLURAL("en", PC::One, 1);
    PLURAL("en", PC::Other, 0, 2, 3, 10, 11, 21, 100, 101, 1000000);
    PLURAL("en-GB", PC::One, 1, -1);
    PLURAL("de_DE.UTF-8", PC::One, 1);
    PLURAL("de", PC::Other, 0, 2, 21);
    PLURAL("es", PC::One, 1);
    PLURAL("it", PC::Other, 0, 5);
    PLURAL("tr", PC::One, 1);
    PLURAL("xx-unknown", PC::One, 1);
    PLURAL("", PC::Other, 2);
    // 0 and 1 are "one"
    PLURAL("fr", PC::One, 0, 1);
    PLURAL("fr", PC::Other, 2, 17, 100);
    PLURAL("pt", PC::One, 0, 1);
    PLURAL("pt-BR", PC::One, 0, 1);
    PLURAL("pt-PT", PC::One, 1);
    PLURAL("pt_PT", PC::Other, 0, 2);
    PLURAL("hi", PC::One, 0, 1);
    PLURAL("hi", PC::Other, 2, 10);
    // no plural
    PLURAL("ja", PC::Other, 0, 1, 2, 100);
    PLURAL("zh-Hans-CN", PC::Other, 0, 1, 5);
    PLURAL("ko", PC::Other, 1);
    PLURAL("vi", PC::Other, 1);
    PLURAL("th", PC::Other, 1);
    PLURAL("id", PC::Other, 1);
    // East Slavic
    for (const char* l : {"ru", "uk", "be", "ru-RU"}) {
        PLURAL(l, PC::One, 1, 21, 31, 101, 1001, 121);
        PLURAL(l, PC::Few, 2, 3, 4, 22, 23, 24, 102, 1004);
        PLURAL(l, PC::Many, 0, 5, 6, 10, 11, 12, 13, 14, 15, 19, 20, 25, 100, 111, 112, 114, 1000);
    }
    // Polish
    PLURAL("pl", PC::One, 1);
    PLURAL("pl", PC::Few, 2, 3, 4, 22, 23, 24, 32, 102);
    PLURAL("pl", PC::Many, 0, 5, 10, 11, 12, 13, 14, 15, 21, 25, 100, 101, 112, 1000);
    // Czech / Slovak
    PLURAL("cs", PC::One, 1);
    PLURAL("cs", PC::Few, 2, 3, 4);
    PLURAL("sk", PC::Other, 0, 5, 10, 21, 22, 100);
    // South Slavic
    PLURAL("sr", PC::One, 1, 21, 101);
    PLURAL("hr", PC::Few, 2, 3, 4, 22, 104);
    PLURAL("bs", PC::Other, 0, 5, 11, 12, 13, 14, 20, 100);
    // Slovenian
    PLURAL("sl", PC::One, 1, 101, 201);
    PLURAL("sl", PC::Two, 2, 102);
    PLURAL("sl", PC::Few, 3, 4, 103, 104);
    PLURAL("sl", PC::Other, 0, 5, 11, 100);
    // Baltic
    PLURAL("lt", PC::One, 1, 21, 31, 101);
    PLURAL("lt", PC::Few, 2, 9, 22, 29, 102);
    PLURAL("lt", PC::Other, 0, 10, 11, 12, 19, 20, 30, 111);
    PLURAL("lv", PC::Zero, 0, 10, 11, 19, 20, 30, 100, 111);
    PLURAL("lv", PC::One, 1, 21, 31, 101);
    PLURAL("lv", PC::Other, 2, 9, 22, 29, 102);
    // Romanian
    PLURAL("ro", PC::One, 1);
    PLURAL("ro", PC::Few, 0, 2, 16, 19, 101, 102, 119);
    PLURAL("ro", PC::Other, 20, 21, 35, 100, 120, 1000);
    // Arabic: all six
    PLURAL("ar", PC::Zero, 0);
    PLURAL("ar", PC::One, 1);
    PLURAL("ar", PC::Two, 2);
    PLURAL("ar", PC::Few, 3, 4, 10, 103, 110, 1005);
    PLURAL("ar", PC::Many, 11, 12, 26, 99, 111, 199, 1011);
    PLURAL("ar", PC::Other, 100, 101, 102, 200, 1000, 1002);
    PLURAL("ar-EG", PC::Few, 7);
    // Hebrew, Welsh, Irish
    PLURAL("he", PC::One, 1);
    PLURAL("he", PC::Two, 2);
    PLURAL("he", PC::Other, 0, 3, 10, 20, 100);
    PLURAL("cy", PC::Zero, 0);
    PLURAL("cy", PC::Few, 3);
    PLURAL("cy", PC::Many, 6);
    PLURAL("cy", PC::Other, 4, 5, 7, 100);
    PLURAL("ga", PC::Two, 2);
    PLURAL("ga", PC::Few, 3, 6);
    PLURAL("ga", PC::Many, 7, 10);
    PLURAL("ga", PC::Other, 0, 11, 100);
    UI_CHECK(pluralCategory("ru", std::numeric_limits<long long>::min()) == PC::Many); // |min| ends in ...808

    // decimals (operands i, v, f)
    UI_CHECK(pluralCategory("en", 1.0, 0) == PC::One);
    UI_CHECK(pluralCategory("en", 1.0, 1) == PC::Other); // "1.0 items"
    UI_CHECK(pluralCategory("en", 1.5, 1) == PC::Other);
    UI_CHECK(pluralCategory("fr", 0.5, 1) == PC::One && pluralCategory("fr", 1.9, 1) == PC::One);
    UI_CHECK(pluralCategory("fr", 2.0, 1) == PC::Other);
    UI_CHECK(pluralCategory("ru", 1.5, 1) == PC::Other && pluralCategory("ru", 21.0, 0) == PC::One);
    UI_CHECK(pluralCategory("cs", 1.5, 1) == PC::Many);
    UI_CHECK(pluralCategory("pl", 2.5, 1) == PC::Other);
    UI_CHECK(pluralCategory("da", 1.0, 1) == PC::One); // n = 1 regardless of the written decimals
    UI_CHECK(pluralCategory("lt", 1.5, 1) == PC::Many);
    UI_CHECK(pluralCategory("ar", 2.0, 0) == PC::Two);
    UI_CHECK(pluralCategory("en", 0.999, 0) == PC::One); // rounds to "1"
    UI_CHECK(pluralCategory("en", std::nan(""), 0) == PC::Other);
    UI_CHECK(pluralCategoryName(PC::Few) == "few" && pluralCategoryName(PC::Other) == "other");
}

void gatePatterns() {
    const char* en = "one {# file} other {# files}";
    UI_CHECK(selectPlural(en, PC::One) == "# file");
    UI_CHECK(selectPlural(en, PC::Many) == "# files"); // falls back to other
    UI_CHECK(formatPlural(en, "en", 1) == "1 file");
    UI_CHECK(formatPlural(en, "en", 0) == "0 files");
    UI_CHECK(formatPlural(en, "en", 12345) == "12,345 files");
    UI_CHECK(formatPlural(en, "de", 12345) == "12.345 files");
    const char* ru = "one{# \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB} few{# \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0} many{# \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2} other{# \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0}";
    UI_CHECK(formatPlural(ru, "ru", 1) == "1 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB");
    UI_CHECK(formatPlural(ru, "ru", 3) == "3 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0");
    UI_CHECK(formatPlural(ru, "ru", 5) == "5 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2");
    UI_CHECK(formatPlural(ru, "ru", 21) == "21 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB");
    UI_CHECK(formatPlural(ru, "ru", 112) == "112 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2");
    // exact matches win; nested braces are kept; '#' in an exact branch is still the number
    const char* cart = "=0 {Your cart is empty} =1 {One item in {cart}} one {# item} other {# items in {cart}}";
    UI_CHECK(formatPlural(cart, "en", 0) == "Your cart is empty");
    UI_CHECK(formatPlural(cart, "en", 1) == "One item in {cart}");
    UI_CHECK(formatPlural(cart, "en", 7) == "7 items in {cart}");
    UI_CHECK(formatPlural("=-1 {below} other {#}", "en", -1) == "below");
    UI_CHECK(formatPlural("other {#}", "en", -1234567) == "-1,234,567");
    // not a pattern: returned whole; a pattern without the category or "other": empty
    UI_CHECK(formatPlural("Plain text", "en", 3) == "Plain text");
    UI_CHECK(selectPlural("Braces {not a keyword pattern", PC::One) == "Braces {not a keyword pattern");
    UI_CHECK(selectPlural("one {x}", PC::Other).empty());
    UI_CHECK(selectPlural("", PC::One).empty());
    std::string out = "> ";
    formatPlural(out, en, "en", 2);
    UI_CHECK(out == "> 2 files");
}

void gateNumbers() {
    const std::string nbsp = "\xC2\xA0", nnbsp = "\xE2\x80\xAF", apos = "\xE2\x80\x99";
    UI_CHECK(formatInteger("en", 0) == "0");
    UI_CHECK(formatInteger("en", 999) == "999");
    UI_CHECK(formatInteger("en", 1000) == "1,000");
    UI_CHECK(formatInteger("en-US", 1234567) == "1,234,567");
    UI_CHECK(formatInteger("en", -1234567) == "-1,234,567");
    UI_CHECK(formatInteger("en", 1234567, false) == "1234567");
    UI_CHECK(formatInteger("de", 1234567) == "1.234.567");
    UI_CHECK(formatInteger("de-CH", 1234567) == "1" + apos + "234" + apos + "567");
    UI_CHECK(formatInteger("fr", 1234567) == "1" + nnbsp + "234" + nnbsp + "567");
    UI_CHECK(formatInteger("ru_RU", 1234567) == "1" + nbsp + "234" + nbsp + "567");
    UI_CHECK(formatInteger("sv", 12345) == "12" + nbsp + "345");
    UI_CHECK(formatInteger("ja", 1234567) == "1,234,567");
    UI_CHECK(formatInteger("hi", 1234567) == "12,34,567"); // Indian grouping
    UI_CHECK(formatInteger("en-IN", 123456789) == "12,34,56,789");
    UI_CHECK(formatInteger("en-IN", 1234) == "1,234");
    UI_CHECK(formatInteger("es", 1234) == "1234"); // at least two digits before the first separator
    UI_CHECK(formatInteger("es", 12345) == "12.345");
    UI_CHECK(formatInteger("es-MX", 1234) == "1,234");
    UI_CHECK(formatInteger("pl", 1234) == "1234" && formatInteger("pl", 12345) == "12" + nbsp + "345");
    UI_CHECK(formatInteger("pt-BR", 1234567) == "1.234.567");
    UI_CHECK(formatInteger("pt-PT", 1234) == "1234" && formatInteger("pt-PT", 1234567) == "1" + nbsp + "234" + nbsp + "567");
    UI_CHECK(formatInteger("xx", 1234) == "1,234"); // unknown: English
    UI_CHECK(formatInteger("en", std::numeric_limits<long long>::min()) == "-9,223,372,036,854,775,808");
    UI_CHECK(formatInteger("en", std::numeric_limits<long long>::max()) == "9,223,372,036,854,775,807");

    UI_CHECK(formatNumber("en", 1234567.891, 2) == "1,234,567.89");
    UI_CHECK(formatNumber("de", 1234567.891, 2) == "1.234.567,89");
    UI_CHECK(formatNumber("fr", 1234567.891, 2) == "1" + nnbsp + "234" + nnbsp + "567,89");
    UI_CHECK(formatNumber("ru", -0.5, 1) == "-0,5");
    UI_CHECK(formatNumber("en", 0.125, 2) == "0.13"); // half away from zero
    UI_CHECK(formatNumber("en", -0.125, 2) == "-0.13");
    UI_CHECK(formatNumber("en", 2.5, 0) == "3" && formatNumber("en", -2.5, 0) == "-3");
    UI_CHECK(formatNumber("en", 999.996, 2) == "1,000.00"); // the carry reaches the grouping
    UI_CHECK(formatNumber("en", -0.001, 2) == "0.00");      // no negative zero
    UI_CHECK(formatNumber("en", 3.0, 3) == "3.000");
    UI_CHECK(formatNumber("en", 1234.5, 1, false) == "1234.5");
    UI_CHECK(formatNumber("en-IN", 1234567.5, 1) == "12,34,567.5");
    UI_CHECK(formatNumber("de", 1e20, 0) == "100.000.000.000.000.000.000");
    UI_CHECK(formatNumber("en", std::nan(""), 2) == "NaN");
    UI_CHECK(formatNumber("en", -std::numeric_limits<double>::infinity(), 2) == "-\xE2\x88\x9E");
    std::string out = "= ";
    formatNumber(out, "de", 42.5, 2);
    UI_CHECK(out == "= 42,50");
    UI_CHECK(numberSymbols("de").decimal == "," && numberSymbols("de").group == ".");
    UI_CHECK(numberSymbols("en-IN").secondaryGroup == 2 && numberSymbols("en").secondaryGroup == 3);
    UI_CHECK(numberSymbols("de-XX").decimal == ","); // unknown region: the language
}

void gatePseudo() {
    const std::string p = pseudoLocalize("Save file");
    UI_CHECK(p.front() == '[' && p.back() == ']');
    UI_CHECK(p.find("Save") == std::string::npos && p.find("file") == std::string::npos); // every word is accented
    UI_CHECK(p.find(' ') != std::string::npos);
    UI_CHECK(graphemeCount(p) >= graphemeCount("Save file") + 2 + 3); // brackets + 30% of 8 letters
    UI_CHECK(p == "[\xC5\xA0\xC3\xA1v\xC3\xA9 \xC6\x92\xC3\xADļ\xC3\xA9 \xC2\xB7\xC2\xB7\xC2\xB7]");
    // every pseudo letter has a glyph in the bundled UI font: a missing one would hide what the transform tests
    {
        const std::string all = pseudoLocalize("abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        for (std::size_t i = 0; i < all.size();) {
            const char32_t cp = decodeUtf8(all, i);
            UI_CHECK(font().glyphIndex(cp) != 0);
        }
    }
    // placeholders, printf conversions, tags, entities and the plural marker survive
    const PseudoOptions plain{.expansion = 0.f, .brackets = false, .accents = true};
    UI_CHECK(pseudoLocalize("{count} of {total}", plain) == "{count} \xC3\xB6\xC6\x92 {total}");
    UI_CHECK(pseudoLocalize("%d%% of %s (%1$s, %.2f, %ld)", plain) == "%d%% \xC3\xB6\xC6\x92 %s (%1$s, %.2f, %ld)");
    UI_CHECK(pseudoLocalize("<b>Go</b> &amp; stop", plain) == "<b>\xC4\x9C\xC3\xB6</b> &amp; \xC5\xA1\xC5\xA3\xC3\xB6p");
    UI_CHECK(pseudoLocalize("one {# file} other {# files}", plain).find("{# file}") != std::string::npos);
    UI_CHECK(pseudoLocalize("a < b", plain) == "\xC3\xA1 < b"); // a bare '<' is text
    UI_CHECK(pseudoLocalize("100%", plain) == "100%");
    UI_CHECK(pseudoLocalize("%1 of %2", plain) == "%1 \xC3\xB6\xC6\x92 %2"); // Qt-style arguments
    // options
    UI_CHECK(pseudoLocalize("Go", {.expansion = 0.f, .brackets = true, .accents = false}) == "[Go]");
    UI_CHECK(pseudoLocalize("Go", {.expansion = 1.f, .brackets = false, .accents = false}) == "Go \xC2\xB7\xC2\xB7");
    UI_CHECK(pseudoLocalize("", {}) == "[]");
    UI_CHECK(pseudoLocalize("123", plain) == "123");
    // non-ASCII input passes through untouched
    UI_CHECK(pseudoLocalize("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", plain) == "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
}

#if FUSE_UI_I18N_HAVE_GAME
void gateStringTable() {
    fuse::ui::game::StringTable t;
    t.add("en", "files=one {# file} other {# files}\ntitle=Files");
    t.add("pl", "files=one {# plik} few {# pliki} many {# plik\xC3\xB3w} other {# pliku}\ntitle=Pliki");
    UI_CHECK(t.trPlural("files", 1) == "1 file" && t.trPlural("files", 2500) == "2,500 files");
    UI_CHECK(t.trPlural("title", 5) == "Files"); // not a plural entry
    UI_CHECK(t.trPlural("missing", 5) == "missing");
    UI_CHECK(t.setLocale("pl"));
    UI_CHECK(t.trPlural("files", 1) == "1 plik" && t.trPlural("files", 3) == "3 pliki");
    UI_CHECK(t.trPlural("files", 5) == "5 plik\xC3\xB3w" && t.trPlural("files", 22) == "22 pliki");
    UI_CHECK(t.trPlural("files", 12345) == "12\xC2\xA0" "345 plik\xC3\xB3w");
}
#endif
} // namespace

int main() {
    gatePlurals();
    gatePatterns();
    gateNumbers();
    gatePseudo();
#if FUSE_UI_I18N_HAVE_GAME
    gateStringTable();
#endif
    return finish("fuse_text_i18n");
}
