// fuse_ui_l10n gate: CLDR plural categories against the CLDR sample values, ICU-style message formatting (plural,
// selectordinal, select, nesting, offset, quoting), locale number / date formatting, StringTable tr with arguments
// and fallback, and fuse_ui_extract_strings on tests/data/l10n (fresh output, merge, idempotent re-run).
#include "test_util.hpp"

#include "fuse/ui/game/game_ui.hpp"
#include "fuse/ui/l10n/l10n.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace l = fuse::ui::l10n;

namespace {

#define NBSP "\xC2\xA0"
#define NNBSP "\xE2\x80\xAF"

int g_checks = 0;

// "1 21 2~4 0.0 1.5": integers a~b expand, decimals keep their visible fraction digits
void checkSamples(const char* loc, bool ordinal, l::Plural want, std::string_view samples) {
    std::istringstream in{std::string(samples)};
    std::string s;
    while (in >> s) {
        std::vector<std::string> values;
        if (const auto t = s.find('~'); t != std::string::npos) {
            for (long v = std::stol(s.substr(0, t)), e = std::stol(s.substr(t + 1)); v <= e; ++v) values.push_back(std::to_string(v));
        } else {
            values.push_back(s);
        }
        for (const std::string& v : values) {
            l::PluralOperands op;
            UI_CHECK(l::PluralOperands::parse(v, op));
            const l::Plural got = ordinal ? l::pluralOrdinal(loc, op) : l::pluralCardinal(loc, op);
            ++g_checks;
            if (got != want) {
                std::fprintf(stderr, "%s %s %s: got %s, want %s\n", loc, ordinal ? "ordinal" : "cardinal", v.c_str(),
                             std::string(l::pluralName(got)).c_str(), std::string(l::pluralName(want)).c_str());
                ++failures();
            }
        }
    }
}

void plurals() {
    using P = l::Plural;
    // operands
    l::PluralOperands op;
    UI_CHECK(l::PluralOperands::parse("1.50", op));
    UI_CHECK(op.n == 1.5 && op.i == 1 && op.v == 2 && op.f == 50 && op.t == 5);
    UI_CHECK(l::PluralOperands::parse("-12.340", op) && op.i == 12 && op.v == 3 && op.f == 340 && op.t == 34);
    UI_CHECK(!l::PluralOperands::parse("1.", op) && !l::PluralOperands::parse("x", op));
    op = l::PluralOperands::fromDouble(1.5);
    UI_CHECK(op.i == 1 && op.v == 1 && op.f == 5);
    op = l::PluralOperands::fromDouble(2.0);
    UI_CHECK(op.i == 2 && op.v == 0);
    UI_CHECK(l::pluralCardinal("en", 1.0) == P::One && l::pluralCardinal("en", 1.5) == P::Other);
    P p{};
    UI_CHECK(l::pluralFromName("few", p) && p == P::Few && !l::pluralFromName("several", p));

    // cardinal, CLDR 44 samples
    for (const char* loc : {"en", "de", "nl", "sv", "fi", "en-US", "de_AT"}) {
        checkSamples(loc, false, P::One, "1");
        checkSamples(loc, false, P::Other, "0 2~16 100 1000 10000 0.0 0.1 1.0 1.5 2.5 1.00");
    }
    checkSamples("fr", false, P::One, "0 1 0.0 0.5 1.5 1.99");
    checkSamples("fr", false, P::Many, "1000000 2000000 3000000");
    checkSamples("fr", false, P::Other, "2~17 100 1000 10000 2.0 3.5 1000000.5");
    checkSamples("es", false, P::One, "1 1.0 1.00");
    checkSamples("es", false, P::Many, "1000000 2000000");
    checkSamples("es", false, P::Other, "0 2~16 100 1000 0.0 0.5 1.5 2.0");
    checkSamples("it", false, P::One, "1");
    checkSamples("it", false, P::Many, "1000000");
    checkSamples("it", false, P::Other, "0 2~16 100 1000 0.0 1.0 1.5");
    checkSamples("pt", false, P::One, "0 1 0.0 1.0 1.5");
    checkSamples("pt", false, P::Many, "1000000");
    checkSamples("pt", false, P::Other, "2~17 100 1000 2.0 3.5");
    checkSamples("pt-PT", false, P::One, "1");
    checkSamples("pt-PT", false, P::Other, "0 2~16 0.0 1.5");
    for (const char* loc : {"ru", "uk"}) {
        checkSamples(loc, false, P::One, "1 21 31 41 51 61 71 81 101 1001");
        checkSamples(loc, false, P::Few, "2~4 22~24 32~34 42~44 102 1002");
        checkSamples(loc, false, P::Many, "0 5~19 100 1000 10000 111 112 114");
        checkSamples(loc, false, P::Other, "0.0 1.0 1.5 10.0 100.0");
    }
    checkSamples("pl", false, P::One, "1");
    checkSamples("pl", false, P::Few, "2~4 22~24 32~34 102");
    checkSamples("pl", false, P::Many, "0 5~19 100 1000 21 31 111 112");
    checkSamples("pl", false, P::Other, "0.0 1.5 10.0");
    checkSamples("cs", false, P::One, "1");
    checkSamples("cs", false, P::Few, "2~4");
    checkSamples("cs", false, P::Many, "0.0 1.5 10.0 100.0");
    checkSamples("cs", false, P::Other, "0 5~19 100 1000");
    checkSamples("ar", false, P::Zero, "0 0.0 0.00");
    checkSamples("ar", false, P::One, "1 1.0 1.00");
    checkSamples("ar", false, P::Two, "2 2.0 2.00");
    checkSamples("ar", false, P::Few, "3~10 103~110 1003 3.0 4.0");
    checkSamples("ar", false, P::Many, "11~26 111 1011 11.0");
    checkSamples("ar", false, P::Other, "100~102 200~202 1000 10000 0.1 0.9 1.1 1.7");
    checkSamples("he", false, P::One, "1 0.0 0.1 0.9");
    checkSamples("he", false, P::Two, "2");
    checkSamples("he", false, P::Other, "0 3~17 100 1000 1.0 1.5 2.0 2.5 10.0");
    for (const char* loc : {"ja", "zh", "ko", "zh-Hant-TW"}) checkSamples(loc, false, P::Other, "0 1 2 15 100 1000 0.0 1.5");
    checkSamples("tr", false, P::One, "1 1.0");
    checkSamples("tr", false, P::Other, "0 2~16 100 0.0 0.5 1.5");
    checkSamples("hi", false, P::One, "0 1 0.0 0.5 1.0");
    checkSamples("hi", false, P::Other, "2~17 100 1.1 2.0");
    checkSamples("xx", false, P::Other, "0 1 2"); // root

    // ordinal
    checkSamples("en", true, P::One, "1 21 31 41 101 1001");
    checkSamples("en", true, P::Two, "2 22 32 102");
    checkSamples("en", true, P::Few, "3 23 33 103");
    checkSamples("en", true, P::Other, "0 4~18 100 111 112 113 1000");
    checkSamples("sv", true, P::One, "1 2 21 22 31 32 101 102");
    checkSamples("sv", true, P::Other, "0 3~17 100 111 112");
    checkSamples("fr", true, P::One, "1");
    checkSamples("fr", true, P::Other, "0 2~16 100");
    checkSamples("it", true, P::Many, "8 11 80 800");
    checkSamples("it", true, P::Other, "0~7 9 10 12~17 100");
    checkSamples("uk", true, P::Few, "3 23 33 43 103");
    checkSamples("uk", true, P::Other, "0~2 4~16 113");
    for (const char* loc : {"de", "es", "ru", "pl", "ja", "ar"}) checkSamples(loc, true, P::Other, "0 1 2 3 11 100");
}

std::string fmt(const char* loc, std::string_view pat, const l::Args& a) {
    auto r = l::formatMessage(loc, pat, a);
    if (!r) {
        std::fprintf(stderr, "format error at %zu in '%.*s': %s\n", r.error().offset, int(pat.size()), pat.data(),
                     r.error().message.c_str());
        return "<error>";
    }
    return *r;
}

#define MSG(loc, pat, args, want)                                                                                      \
    do {                                                                                                               \
        const std::string got_ = fmt(loc, pat, args);                                                                  \
        if (got_ != (want)) {                                                                                          \
            std::fprintf(stderr, "%s:%d: '%s' -> '%s', want '%s'\n", __FILE__, __LINE__, pat, got_.c_str(),          \
                         std::string(want).c_str());                                                                   \
            ++failures();                                                                                              \
        }                                                                                                              \
    } while (0)

void messages() {
    const char* items = "{count, plural, =0 {No items} one {# item} other {# items}}";
    MSG("en", items, l::Args({{"count", 0}}), "No items");
    MSG("en", items, l::Args({{"count", 1}}), "1 item");
    MSG("en", items, l::Args({{"count", 2}}), "2 items");
    MSG("en", items, l::Args({{"count", 1000}}), "1,000 items");
    MSG("en", items, l::Args({{"count", 1.5}}), "1.5 items");
    MSG("en", items, l::Args({{"count", "1.0"}}), "1.0 items"); // v = 1: other
    MSG("en", items, l::Args({{"count", "1"}}), "1 item");
    MSG("de", items, l::Args({{"count", 1000}}), "1.000 items");
    MSG("de", items, l::Args({{"count", "1.50"}}), "1,50 items");
    const char* files = "{n, plural, one {# файл} few {# файла} many {# файлов} other {# файла}}";
    MSG("ru", files, l::Args({{"n", 1}}), "1 файл");
    MSG("ru", files, l::Args({{"n", 3}}), "3 файла");
    MSG("ru", files, l::Args({{"n", 5}}), "5 файлов");
    MSG("ru", files, l::Args({{"n", 21}}), "21 файл");
    MSG("ru", files, l::Args({{"n", 1.5}}), "1,5 файла");
    MSG("ar", "{n, plural, zero {لا شيء} one {واحد} two {اثنان} few {# قليلة} many {# كثيرة} other {#}}",
        l::Args({{"n", 3}}), "3 قليلة");

    // offset: '#' and keywords use n - offset, exact matches the raw value
    const char* party = "{n, plural, offset:1 =0 {Nobody came} =1 {{host} came alone} one {{host} and # other came} "
                        "other {{host} and # others came}}";
    MSG("en", party, l::Args({{"n", 0}, {"host", "Ann"}}), "Nobody came");
    MSG("en", party, l::Args({{"n", 1}, {"host", "Ann"}}), "Ann came alone");
    MSG("en", party, l::Args({{"n", 2}, {"host", "Ann"}}), "Ann and 1 other came");
    MSG("en", party, l::Args({{"n", 3}, {"host", "Ann"}}), "Ann and 2 others came");

    // select, missing and positional arguments
    const char* liked = "{g, select, male {He} female {She} other {They}} liked it";
    MSG("en", liked, l::Args({{"g", "female"}}), "She liked it");
    MSG("en", liked, l::Args({{"g", "x"}}), "They liked it");
    MSG("en", liked, l::Args(), "{g} liked it");
    MSG("en", "Hi {name}, {0} of {1}", l::Args({{"0", 3}, {"1", 10}}), "Hi {name}, 3 of 10");

    // nesting: plural inside select; '#' belongs to the innermost plural, and is literal inside a nested select
    const char* cats = "{g, select, female {{n, plural, one {She has # cat} other {She has # cats}}} "
                       "other {{n, plural, one {They have # cat} other {They have # cats}}}}";
    MSG("en", cats, l::Args({{"g", "female"}, {"n", 2}}), "She has 2 cats");
    MSG("en", cats, l::Args({{"g", "male"}, {"n", 1}}), "They have 1 cat");
    MSG("en", "{n, plural, other {{g, select, other {#}}}}", l::Args({{"n", 4}, {"g", "x"}}), "#");
    MSG("en", "{n, plural, other {# {m, plural, one {# day} other {# days}}}}", l::Args({{"n", 4}, {"m", 1}}), "4 1 day");
    MSG("en", "Item #{n}", l::Args({{"n", 3}}), "Item #3");
    MSG("en", "{n, plural, other {# ({p, number, percent})}}", l::Args({{"n", 2}, {"p", 0.5}}), "2 (50%)");

    // apostrophe quoting
    MSG("en", "I''m {name}", l::Args({{"name", "Ann"}}), "I'm Ann");
    MSG("en", "It's {name}", l::Args({{"name", "Ann"}}), "It's Ann");
    MSG("en", "'{name}' is literal", l::Args({{"name", "Ann"}}), "{name} is literal");
    MSG("en", "'{'{name}'}'", l::Args({{"name", "Ann"}}), "{Ann}");
    MSG("en", "don''t '{quote}' ''{name}''", l::Args({{"name", "Ann"}}), "don't {quote} 'Ann'");
    MSG("en", "'{it''s}'", l::Args(), "{it's}");
    MSG("en", "{n, plural, other {'#' is #}}", l::Args({{"n", 5}}), "# is 5");
    MSG("en", "'#' {n}", l::Args({{"n", 5}}), "'#' 5"); // '#' not special outside plural: apostrophe stays
    MSG("en", "unterminated '{quote", l::Args(), "unterminated {quote");

    // selectordinal
    const char* ord = "{n, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}";
    const std::pair<int, const char*> ords[] = {{1, "1st"},  {2, "2nd"},   {3, "3rd"},   {4, "4th"},   {11, "11th"},
                                                {12, "12th"}, {13, "13th"}, {21, "21st"}, {102, "102nd"}, {111, "111th"}};
    for (const auto& [n, want] : ords) MSG("en", ord, l::Args({{"n", n}}), want);

    // number and date arguments
    MSG("en", "{x, number}", l::Args({{"x", 1234.5678}}), "1,234.568");
    MSG("en", "{x, number, integer}", l::Args({{"x", 1234.5}}), "1,234");
    MSG("en", "{x, number, percent}", l::Args({{"x", 0.256}}), "26%");
    MSG("de", "{x, number, percent}", l::Args({{"x", 0.256}}), "26" NBSP "%");
    MSG("en", "{x}", l::Args({{"x", 1234567}}), "1,234,567");
    const l::Date d{2024, 3, 5};
    MSG("en", "{d, date, long}", l::Args({{"d", d}}), "March 5, 2024");
    MSG("en", "{d, date, short}", l::Args({{"d", d}}), "3/5/24");
    MSG("en", "{d, date}", l::Args({{"d", d}}), "Mar 5, 2024");
    MSG("de", "Am {d, date, long}", l::Args({{"d", d}}), "Am 5. März 2024");

    // errors
    for (const char* bad : {"{n, plural, one {x}}", "{unclosed", "a } b", "{n, foo}", "{n, number, ::compact}",
                            "{n, plural, one x}", "{, plural, other {x}}", "{n, select, other {x}", "{n, date, weird}"}) {
        UI_CHECK(!l::formatMessage("en", bad, l::Args({{"n", 1}})));
        UI_CHECK(l::formatMessageOr("en", bad, l::Args({{"n", 1}})) == bad);
    }
}

void numbers() {
    const double big = 1234567.891;
    UI_CHECK_EQ(l::formatNumber("en", big), std::string("1,234,567.891"));
    UI_CHECK_EQ(l::formatNumber("de", big), std::string("1.234.567,891"));
    UI_CHECK_EQ(l::formatNumber("fr", big), std::string("1" NNBSP "234" NNBSP "567,891"));
    UI_CHECK_EQ(l::formatNumber("ru", big), std::string("1" NBSP "234" NBSP "567,891"));
    UI_CHECK_EQ(l::formatNumber("it", big), std::string("1.234.567,891"));
    UI_CHECK_EQ(l::formatNumber("pt-BR", big), std::string("1.234.567,891"));
    UI_CHECK_EQ(l::formatNumber("ja", big), std::string("1,234,567.891"));
    UI_CHECK_EQ(l::formatNumber("ar", big), std::string("1,234,567.891"));
    UI_CHECK_EQ(l::formatNumber("de-CH", 1234567.5), std::string("1\xE2\x80\x99" "234\xE2\x80\x99" "567.5"));
    UI_CHECK_EQ(l::formatNumber("sv", -1234.5), std::string("\xE2\x88\x92" "1" NBSP "234,5"));
    UI_CHECK_EQ(l::formatNumber("es", 1234), std::string("1234")); // minimum grouping digits 2
    UI_CHECK_EQ(l::formatNumber("es", 12345), std::string("12.345"));
    UI_CHECK_EQ(l::formatNumber("pl", 1234), std::string("1234"));
    UI_CHECK_EQ(l::formatNumber("pl", 12345), std::string("12" NBSP "345"));
    UI_CHECK_EQ(l::formatNumber("hi", 1234567.5), std::string("12,34,567.5")); // lakh grouping
    UI_CHECK_EQ(l::formatNumber("en-IN", 123456789), std::string("12,34,56,789"));
    UI_CHECK_EQ(l::formatNumber("en_in", 1000), std::string("1,000"));
    UI_CHECK_EQ(l::formatNumber("xx", 1234.5), std::string("1,234.5"));
    // rounding: half-even on the shortest decimal
    UI_CHECK_EQ(l::formatNumber("en", 0.125, {0, 2}), std::string("0.12"));
    UI_CHECK_EQ(l::formatNumber("en", 0.135, {0, 2}), std::string("0.14"));
    UI_CHECK_EQ(l::formatNumber("en", 2.5, {0, 0}), std::string("2"));
    UI_CHECK_EQ(l::formatNumber("en", 3.5, {0, 0}), std::string("4"));
    UI_CHECK_EQ(l::formatNumber("en", 9.9996), std::string("10"));
    UI_CHECK_EQ(l::formatNumber("en", 999999.9999, {0, 3}), std::string("1,000,000"));
    UI_CHECK_EQ(l::formatNumber("en", -0.0001), std::string("0"));
    UI_CHECK_EQ(l::formatNumber("en", -2.5), std::string("-2.5"));
    UI_CHECK_EQ(l::formatNumber("de", 5, {2, 2}), std::string("5,00"));
    UI_CHECK_EQ(l::formatNumber("en", 12345, {0, 3, false}), std::string("12345"));
    UI_CHECK_EQ(l::formatNumber("en", 0.1 + 0.2), std::string("0.3"));
    // percent
    UI_CHECK_EQ(l::formatPercent("en", 0.5), std::string("50%"));
    UI_CHECK_EQ(l::formatPercent("de", 0.5), std::string("50" NBSP "%"));
    UI_CHECK_EQ(l::formatPercent("fr", 0.5), std::string("50" NNBSP "%"));
    UI_CHECK_EQ(l::formatPercent("tr", 0.5), std::string("%50"));
    UI_CHECK_EQ(l::formatPercent("tr", -0.5), std::string("-%50"));
    UI_CHECK_EQ(l::formatPercent("en", 12.25, 1), std::string("1,225%"));
    UI_CHECK_EQ(l::formatPercent("hi", 1234.5), std::string("1,23,450%"));
    UI_CHECK(l::decimalSeparator("de-AT") == "," && l::groupingSeparator("de-AT") == NBSP);
    UI_CHECK(l::decimalSeparator("en") == "." && l::groupingSeparator("fr-FR") == NNBSP);
}

void dates() {
    const l::Date d{2024, 3, 5};
    using S = l::DateStyle;
    struct Case { const char* loc; S style; const char* want; };
    const Case cases[] = {
        {"en", S::Long, "March 5, 2024"},         {"en", S::Medium, "Mar 5, 2024"},          {"en", S::Short, "3/5/24"},
        {"en-GB", S::Long, "5 March 2024"},       {"en-GB", S::Short, "05/03/2024"},
        {"de", S::Long, "5. März 2024"},           {"de", S::Medium, "05.03.2024"},            {"de", S::Short, "05.03.24"},
        {"fr", S::Long, "5 mars 2024"},            {"fr", S::Medium, "5 mars 2024"},           {"fr", S::Short, "05/03/2024"},
        {"es", S::Long, "5 de marzo de 2024"},     {"es", S::Medium, "5 mar 2024"},            {"es", S::Short, "5/3/24"},
        {"it", S::Long, "5 marzo 2024"},           {"pt", S::Long, "5 de março de 2024"},      {"nl", S::Short, "05-03-2024"},
        {"sv", S::Short, "2024-03-05"},            {"pl", S::Long, "5 marca 2024"},            {"tr", S::Long, "5 Mart 2024"},
        {"ru", S::Long, "5 марта 2024 г."},        {"ru", S::Medium, "5 мар. 2024 г."},        {"ru", S::Short, "05.03.2024"},
        {"ja", S::Long, "2024年3月5日"},           {"ja", S::Short, "2024/03/05"},             {"zh", S::Short, "2024/3/5"},
        {"ko", S::Long, "2024년 3월 5일"},         {"ko", S::Short, "24. 3. 5."},
        {"ar", S::Long, "5 مارس 2024"},            {"ar", S::Short, "5\xE2\x80\x8F/3\xE2\x80\x8F/2024"},
        {"xx", S::Long, "March 5, 2024"},
    };
    for (const Case& c : cases) {
        const std::string got = l::formatDate(c.loc, d, c.style);
        if (got != c.want) {
            std::fprintf(stderr, "date %s %d: '%s', want '%s'\n", c.loc, int(c.style), got.c_str(), c.want);
            ++failures();
        }
    }
    UI_CHECK_EQ(l::formatDate("en", {2009, 12, 31}, S::Short), std::string("12/31/09"));
    UI_CHECK_EQ(l::formatDate("de", {2009, 12, 31}, S::Long), std::string("31. Dezember 2009"));
    UI_CHECK(l::monthName("fr", 8) == "août" && l::monthName("de", 9, true) == "Sept." && l::monthName("en", 13).empty());
}

void stringTable() {
    namespace g = fuse::ui::game;
    g::StringTable st;
    st.add("en", "items={n, plural, one {# item} other {# items}}\ngreet=Hello, {name}!\nonly.en=English only\n"
                 "quote=It''s {name}\nmulti=line1\\nline2\nkey\\=eq=v\n# comment=no\n");
    st.add("de", "items={n, plural, one {# Gegenstand} other {# Gegenst\xC3\xA4nde}}\ngreet=Hallo, {name}!");
    st.add("ru", "items={n, plural, one {# \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBC\xD0\xB5\xD1\x82} "
                 "few {# \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBC\xD0\xB5\xD1\x82\xD0\xB0} "
                 "many {# \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBC\xD0\xB5\xD1\x82\xD0\xBE\xD0\xB2} other {# x}}");
    UI_CHECK(st.locale() == "en" && st.fallbackLocale() == "en");
    UI_CHECK_EQ(st.tr("items", {{"n", 1}}), std::string("1 item"));
    UI_CHECK_EQ(st.tr("items", {{"n", 1000}}), std::string("1,000 items"));
    UI_CHECK_EQ(st.tr("quote", {{"name", "Ann"}}), std::string("It's Ann"));
    UI_CHECK(st.tr("quote") == "It''s {name}"); // raw value without arguments
    UI_CHECK(st.tr("multi") == "line1\nline2" && st.tr("key=eq") == "v" && st.tr("# comment") == "# comment");
    UI_CHECK(st.setLocale("de"));
    UI_CHECK_EQ(st.tr("items", {{"n", 1000}}), std::string("1.000 Gegenst\xC3\xA4nde"));
    UI_CHECK_EQ(st.tr("greet", {{"name", "Ann"}}), std::string("Hallo, Ann!"));
    UI_CHECK(st.tr("only.en") == "English only"); // fallback locale
    UI_CHECK_EQ(st.tr("only.en", {}), std::string("English only"));
    UI_CHECK_EQ(st.tr("missing {n, plural, one {# day} other {# days}}", {{"n", 1}}), std::string("missing 1 day"));
    UI_CHECK(st.tr("missing.key") == "missing.key");
    UI_CHECK(st.setLocale("ru"));
    UI_CHECK_EQ(st.tr("items", {{"n", 22}}), std::string("22 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBC\xD0\xB5\xD1\x82\xD0\xB0"));
    UI_CHECK_EQ(st.tr("items", {{"n", 5}}), std::string("5 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBC\xD0\xB5\xD1\x82\xD0\xBE\xD0\xB2"));
    UI_CHECK_EQ(st.tr("greet", {{"name", "Ann"}}), std::string("Hello, Ann!")); // en string, en rules
    st.setFallbackLocale("de");
    UI_CHECK_EQ(st.tr("greet", {{"name", "Ann"}}), std::string("Hallo, Ann!"));
    UI_CHECK(st.has("de", "greet") && !st.has("ru", "greet"));
    st.add("de", "greet=Servus, {name}!"); // replaces
    UI_CHECK_EQ(st.tr("greet", {{"name", "Ann"}}), std::string("Servus, Ann!"));

    // bound nodes with arguments re-text on refresh
    UiContext ui({.viewport = {200, 100}, .font = &font()});
    widgets::install(ui);
    const NodeHandle lbl = *ui.create("label", ui.root());
    st.setLocale("en");
    st.bind(ui, lbl, "items", l::Args{{"n", 3}});
    UI_CHECK(ui.text(lbl) == "3 items");
    st.setLocale("de");
    st.refresh(ui);
    UI_CHECK(ui.text(lbl) == "3 Gegenst\xC3\xA4nde");
}

std::string readAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    std::erase(s, '\r');
    return s;
}
void writeAll(const std::string& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

int run(const std::string& args, const std::string& log) {
    std::string cmd = "\"" + std::string(FUSE_UI_EXTRACT_EXE) + "\" " + args + " > \"" + log + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\""; // cmd.exe /c strips one outer pair of quotes
#endif
    return std::system(cmd.c_str());
}

void extraction() {
    const std::string dir = FUSE_UI_TEST_OUT;
    const std::string table = dir + "/l10n_de.strings", log = dir + "/l10n_extract.log";
    writeAll(table, readAll("data/l10n/de.strings"));
    const std::string args = "--out=\"" + table + "\" --locale=de data/l10n/sample.fuseui data/l10n/sample.cpp";
    UI_CHECK_EQ(run(args, log), 0);
    const std::string want =
        "# fuse_ui string table: de\n"
        "# written by fuse_ui_extract_strings: key=value, sorted by key; #? untranslated, #, new, #~ obsolete\n"
        "\n#: data/l10n/sample.fuseui:back\nBack=Zur\xC3\xBC" "ck\n"
        "\n#: data/l10n/sample.fuseui:back\n#, new\n#? Go back\n"
        "\n#: data/l10n/sample.cpp:6\n#, new\n#? Hello, \"world\"\\n\n"
        "\n#: data/l10n/sample.fuseui:quality\nHigh=Hoch\n"
        "\n#: data/l10n/sample.fuseui:title\n#, new\n#? Inventory\n"
        "\n#: data/l10n/sample.fuseui:quality\n#? Low\n"
        "\n#: data/l10n/sample.cpp:7\n#, new\n#? adjacent literals joined\n"
        "\n#: data/l10n/sample.cpp:16\n#, new\n#? after.quote\n"
        "\n#: data/l10n/sample.cpp:9\n#, new\n#? caf\xC3\xA9\n"
        "\n#: data/l10n/sample.fuseui:root/children[5]/children[0]\n#, new\n#? key\\=with\\nnewline\n"
        "\n#: data/l10n/sample.cpp:5\n#: data/l10n/sample.cpp:18\nmenu.title=Inventar\n"
        "\n#: data/l10n/sample.cpp:10\n#, new\n#? raw {n, plural, one {# item} other {# items}}\n"
        "\n# obsolete: no longer in the sources\n#~ old.key=Alt\n";
    const std::string got = readAll(table);
    if (got != want) {
        std::fprintf(stderr, "extracted table differs:\n---- got\n%s---- want\n%s", got.c_str(), want.c_str());
        ++failures();
    }
    const std::string report = readAll(log);
    UI_CHECK(report.find("12 strings, 3 translated, 9 untranslated, 8 new, 1 obsolete") != std::string::npos);
    UI_CHECK(report.find("new: Inventory\n") != std::string::npos && report.find("obsolete: old.key\n") != std::string::npos);

    // the merged table loads: translations kept, untranslated keys fall back
    fuse::ui::game::StringTable st;
    st.add("en", "Inventory=Inventory");
    st.add("de", got);
    st.setLocale("de");
    UI_CHECK(st.tr("Back") == "Zur\xC3\xBC" "ck" && st.tr("High") == "Hoch" && st.tr("Inventory") == "Inventory");
    UI_CHECK(!st.has("de", "old.key") && !st.has("de", "Low"));

    // a translator fills one in; the re-run keeps it, flags nothing new and is otherwise stable
    std::string edited = got;
    edited.replace(edited.find("#? Inventory\n"), 13, "Inventory=Inventar\n");
    writeAll(table, edited);
    UI_CHECK_EQ(run(args, log), 0);
    std::string want2 = want;
    want2.erase(want2.find("#, new\n#? Inventory\n"), 7);
    std::size_t at;
    while ((at = want2.find("#, new\n")) != std::string::npos) want2.erase(at, 7);
    want2.replace(want2.find("#? Inventory\n"), 13, "Inventory=Inventar\n");
    UI_CHECK(readAll(table) == want2);
    UI_CHECK(readAll(log).find("12 strings, 4 translated, 8 untranslated, 0 new, 1 obsolete") != std::string::npos);
    UI_CHECK_EQ(run(args, log), 0);
    UI_CHECK(readAll(table) == want2); // deterministic

    // source-language table, directory input, no locations, obsolete dropped
    const std::string src = dir + "/l10n_en.strings";
    writeAll(src, "gone=Gone\n");
    UI_CHECK_EQ(run("--out=\"" + src + "\" --source --no-locations --drop-obsolete data/l10n", log), 0);
    const std::string en = readAll(src);
    UI_CHECK(en.find("\n#, new\nInventory=Inventory\n") != std::string::npos);
    UI_CHECK(en.find("\n#, new\nmenu.title=menu.title\n") != std::string::npos);
    UI_CHECK(en.find("#:") == std::string::npos && en.find("gone") == std::string::npos);
    UI_CHECK(readAll(log).find("12 strings, 12 translated, 0 untranslated, 12 new, 1 obsolete (dropped)") != std::string::npos);
    // --fn picks other function names
    UI_CHECK_EQ(run("--out=\"" + src + "\" --source --no-locations --drop-obsolete --fn=str data/l10n/sample.cpp", log), 0);
    UI_CHECK(readAll(src).find("not.tr=not.tr") != std::string::npos && readAll(src).find("menu.title") == std::string::npos);
    // a bad document fails
    writeAll(dir + "/l10n_bad.fuseui", "{ \"root\": ");
    UI_CHECK(run("--out=\"" + src + "\" \"" + dir + "/l10n_bad.fuseui\"", log) != 0);
}

} // namespace

int main() {
    plurals();
    messages();
    numbers();
    dates();
    stringTable();
    extraction();
    std::printf("fuse_ui_l10n: %d plural samples checked\n", g_checks);
    return finish("fuse_ui_l10n");
}
