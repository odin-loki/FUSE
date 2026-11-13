// fuse_ui_l10n_po: gettext .po catalogues read into string-table entries (l10n::parsePo): header language, contexts,
// multi-line strings and escapes, fuzzy / untranslated entries skipped, plural entries turned into ICU plural messages
// on the CLDR categories from the catalogue's own Plural-Forms expression (Polish: 3 forms over one / few / many /
// other; English: 2; Japanese: 1), malformed input; and the entries used through StringTable (with fuse_ui_game).
#include "fuse/ui/l10n/l10n.hpp"

#if FUSE_UI_L10N_PO_HAVE_GAME
#include "fuse/ui/game/game_ui.hpp"
#endif

#include <cstdio>
#include <string>

namespace l10n = fuse::ui::l10n;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
const l10n::TableEntry* find(const l10n::PoCatalog& c, std::string_view key) {
    for (const auto& e : c.entries)
        if (e.key == key) return &e;
    return nullptr;
}
std::string plural(const l10n::PoCatalog& c, std::string_view key, long long n) {
    const l10n::TableEntry* e = find(c, key);
    return e ? l10n::formatMessageOr(c.language, e->value, {{"n", n}}) : std::string("<missing>");
}
} // namespace

int main() {
    const char* po =
        "# translator comment\n"
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Project-Id-Version: demo\\n\"\n"
        "\"Language: pl\\n\"\n"
        "\"Plural-Forms: nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\\n\"\n"
        "\n"
        "#: ui/main.fuseui:12\n"
        "msgctxt \"menu\"\n"
        "msgid \"Open\"\n"
        "msgstr \"Otworz\"\n"
        "\n"
        "msgid \"Open\"\n"
        "msgstr \"Otworz plik\"\n"
        "#, fuzzy\n" // no blank line before the next entry
        "msgid \"Close\"\n"
        "msgstr \"Zamknij\"\n"
        "\n"
        "msgid \"Quit\"\n"
        "msgstr \"\"\n"
        "\n"
        "msgid \"%n file\"\n"
        "msgid_plural \"%n files\"\n"
        "msgstr[0] \"%n plik\"\n"
        "msgstr[1] \"%n pliki\"\n"
        "msgstr[2] \"%n plikow\"\n"
        "\n"
        "msgid \"multi\"\n"
        "msgstr \"\"\n"
        "\"line \\\"one\\\" \"\n"
        "\"two\\n\"\n"
        "\n"
        "msgid \"braces\"\n"
        "msgid_plural \"braces\"\n"
        "msgstr[0] \"{%n} it's #1\"\n"
        "msgstr[1] \"{%n} it's #2\"\n"
        "msgstr[2] \"{%n} it's #5\"\n"
        "#~ msgid \"gone\"\n"
        "#~ msgstr \"dawno\"\n";
    auto parsed = l10n::parsePo(po);
    CHECK(parsed.has_value());
    if (!parsed) {
        std::fprintf(stderr, "parsePo: %s at %zu\n", parsed.error().message.c_str(), parsed.error().offset);
        return 1;
    }
    const l10n::PoCatalog& c = *parsed;
    CHECK(c.language == "pl");
    CHECK(c.skippedFuzzy == 1 && c.untranslated == 1);
    CHECK(c.entries.size() == 5);
    CHECK(find(c, l10n::poKey("menu", "Open")) && find(c, l10n::poKey("menu", "Open"))->value == "Otworz");
    CHECK(find(c, "Open") && find(c, "Open")->value == "Otworz plik");
    CHECK(!find(c, "Close") && !find(c, "Quit") && !find(c, "gone"));
    CHECK(find(c, "multi") && find(c, "multi")->value == "line \"one\" two\n");
    // Polish: 1 plik, 2-4 pliki, 5-21 plikow, 22-24 pliki, 25 plikow, 112 plikow, 1000000 plikow
    CHECK(plural(c, "%n file", 1) == "1 plik");
    CHECK(plural(c, "%n file", 2) == "2 pliki" && plural(c, "%n file", 4) == "4 pliki");
    CHECK(plural(c, "%n file", 5) == "5 plikow" && plural(c, "%n file", 12) == "12 plikow" && plural(c, "%n file", 21) == "21 plikow");
    CHECK(plural(c, "%n file", 22) == "22 pliki" && plural(c, "%n file", 25) == "25 plikow" && plural(c, "%n file", 112) == "112 plikow");
    CHECK(plural(c, "%n file", 0) == "0 plikow");
    CHECK(find(c, "%n file")->value.find("one {# plik}") != std::string::npos && find(c, "%n file")->value.find("few {# pliki}") != std::string::npos);
    // ICU syntax characters of a translation are literal text
    CHECK(plural(c, "braces", 1) == "{1} it's #1" && plural(c, "braces", 3) == "{3} it's #2" && plural(c, "braces", 7) == "{7} it's #5");

    // the table text round-trips through the "key=value" reader
    const std::vector<l10n::TableEntry> table = l10n::parseTable(l10n::poToTable(c));
    CHECK(table.size() == c.entries.size());
    for (std::size_t i = 0; i < table.size() && i < c.entries.size(); ++i) CHECK(table[i].key == c.entries[i].key && table[i].value == c.entries[i].value);

    // English (two forms, the usual expression) and Japanese (one form); no header: the Germanic rule, language empty
    auto en = l10n::parsePo("msgid \"\"\nmsgstr \"Language: en\\nPlural-Forms: nplurals=2; plural=(n != 1);\\n\"\n\n"
                            "msgid \"%d apple\"\nmsgid_plural \"%d apples\"\nmsgstr[0] \"%d apple\"\nmsgstr[1] \"%d apples\"\n");
    CHECK(en && plural(*en, "%d apple", 1) == "1 apple" && plural(*en, "%d apple", 0) == "0 apples" && plural(*en, "%d apple", 1234) == "1,234 apples");
    auto ja = l10n::parsePo("msgid \"\"\nmsgstr \"Language: ja\\nPlural-Forms: nplurals=1; plural=0;\\n\"\n\n"
                            "msgid \"%n file\"\nmsgid_plural \"%n files\"\nmsgstr[0] \"%n ko\"\n");
    CHECK(ja && plural(*ja, "%n file", 1) == "1 ko" && plural(*ja, "%n file", 7) == "7 ko");
    auto bare = l10n::parsePo("msgid \"a\"\nmsgid_plural \"as\"\nmsgstr[0] \"one a\"\nmsgstr[1] \"many as\"\n");
    CHECK(bare && bare->language.empty() && l10n::formatMessageOr("en", find(*bare, "a")->value, {{"n", 1}}) == "one a" &&
          l10n::formatMessageOr("en", find(*bare, "a")->value, {{"n", 3}}) == "many as");
    // a plural entry translated only in part falls back to the source text for the missing form
    auto part = l10n::parsePo("msgid \"\"\nmsgstr \"Language: en\\nPlural-Forms: nplurals=2; plural=(n != 1);\\n\"\n\n"
                              "msgid \"%n cat\"\nmsgid_plural \"%n cats\"\nmsgstr[0] \"%n kot\"\nmsgstr[1] \"\"\n");
    CHECK(part && plural(*part, "%n cat", 1) == "1 kot" && plural(*part, "%n cat", 2) == "2 cats");

    // malformed input fails with the offset of the line
    auto bad1 = l10n::parsePo("msgid \"a\"\nmsgstr \"unterminated\n");
    CHECK(!bad1 && bad1.error().offset == 10);
    auto bad2 = l10n::parsePo("\"stray\"\n");
    CHECK(!bad2 && bad2.error().offset == 0);
    auto bad3 = l10n::parsePo("msgid \"a\"\nmsgstr \"b\"\n\nfoo \"x\"\n");
    CHECK(!bad3 && bad3.error().message.find("unknown") != std::string::npos);
    CHECK(l10n::parsePo("").has_value() && l10n::parsePo("")->entries.empty());

#if FUSE_UI_L10N_PO_HAVE_GAME
    // through the game StringTable: the catalogue's language as the locale
    fuse::ui::game::StringTable st;
    st.add(c.language, l10n::poToTable(c));
    st.setLocale(c.language);
    CHECK(st.tr("Open") == "Otworz plik");
    CHECK(st.tr(l10n::poKey("menu", "Open")) == "Otworz");
    CHECK(st.tr("%n file", {{"n", 3}}) == "3 pliki" && st.tr("%n file", {{"n", 17}}) == "17 plikow");
    CHECK(st.tr("Close") == "Close"); // fuzzy: not in the table, falls back to the key
#endif

    std::printf("fuse_ui_l10n_po: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
