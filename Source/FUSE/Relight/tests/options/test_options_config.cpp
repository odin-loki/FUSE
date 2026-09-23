// rl_options / config: .conf syntax, value parsing and formatting, parse errors and edge cases,
// hash-set syntax, byte-stable serialization. Expected values follow dxvk-remix
// src/util/config/config.cpp and src/util/util_hash_set_layer.h@0867d3c (and the ported checks of
// test_rtx_option.cpp test_configParsing / test_configScalarToVectorPromotion / test_hashSetLayerDirect).

#include "rl_options_test.hpp"

#include <fuse/core/temp_path.hpp>
#include <fuse/relight/options/options.hpp>

#include <clocale>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace rl_options_test {
namespace {

using namespace fuse::relight::options;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool hasDiagnostic(const std::vector<ConfigDiagnostic>& diagnostics, std::uint32_t line, const std::string& fragment) {
    for (const ConfigDiagnostic& d : diagnostics) {
        if (d.line == line && d.message.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

ConfigParseOptions exe(const char* name) {
    ConfigParseOptions options;
    options.exeName = name;
    return options;
}

// ----------------------------------------------------------------------------
// Scalar parsing
// ----------------------------------------------------------------------------

void testBoolParsing() {
    bool b = false;
    RL_CHECK(parseOptionValue("True", b) && b);
    RL_CHECK(parseOptionValue("false", b) && !b);
    RL_CHECK(parseOptionValue("1", b) && b);
    RL_CHECK(parseOptionValue("0", b) && !b);
    RL_CHECK(parseOptionValue("TRUE", b) && b);
    RL_CHECK(parseOptionValue("fAlSe", b) && !b);
    // Failures leave the value untouched.
    b = true;
    RL_CHECK(!parseOptionValue("yes", b) && b);
    RL_CHECK(!parseOptionValue("True ", b)); // Remix does not trim values
    RL_CHECK(!parseOptionValue(" 1", b));
    RL_CHECK(!parseOptionValue("", b));
    RL_CHECK(!parseOptionValue("2", b));
}

void testIntParsing() {
    std::int32_t i = 7;
    RL_CHECK(parseOptionValue("42", i) && i == 42);
    RL_CHECK(parseOptionValue("-100", i) && i == -100);
    RL_CHECK(parseOptionValue("  +7", i) && i == 7);     // leading whitespace and '+', as std::stoi
    RL_CHECK(parseOptionValue("12abc", i) && i == 12);   // trailing text ignored
    RL_CHECK(parseOptionValue("0x10", i) && i == 0);     // base 10: stops at 'x'
    RL_CHECK(parseOptionValue("-2147483648", i) && i == std::numeric_limits<std::int32_t>::min());
    RL_CHECK(parseOptionValue("2147483647", i) && i == std::numeric_limits<std::int32_t>::max());
    i = 5;
    RL_CHECK(!parseOptionValue("2147483648", i) && i == 5); // out of range
    RL_CHECK(!parseOptionValue("abc", i) && i == 5);
    RL_CHECK(!parseOptionValue("", i));
    RL_CHECK(!parseOptionValue("-", i));
    RL_CHECK(!parseOptionValue("99999999999999999999999", i));

    std::uint32_t u = 0;
    RL_CHECK(parseOptionValue("4000000000", u) && u == 4000000000u);
    RL_CHECK(parseOptionValue("-1", u) && u == 0xFFFFFFFFu); // std::stol then narrowed
}

void testFloatParsing() {
    float f = 0.0f;
    RL_CHECK(parseOptionValue("3.14159", f) && f == 3.14159f);
    RL_CHECK(parseOptionValue("-2.5", f) && f == -2.5f);
    RL_CHECK(parseOptionValue(" 1e-05", f) && f == 1e-05f);
    RL_CHECK(parseOptionValue("+.5", f) && f == 0.5f);
    RL_CHECK(parseOptionValue("1.5f", f) && f == 1.5f); // trailing text ignored, as std::stof
    RL_CHECK(parseOptionValue("7", f) && f == 7.0f);
    RL_CHECK(parseOptionValue("0x1p3", f) && f == 8.0f); // hex float
    RL_CHECK(parseOptionValue("0xg", f) && f == 0.0f);   // "0" then stops
    RL_CHECK(parseOptionValue("inf", f) && std::isinf(f));
    f = 9.0f;
    RL_CHECK(!parseOptionValue("abc", f) && f == 9.0f);
    RL_CHECK(!parseOptionValue("", f));
    RL_CHECK(!parseOptionValue("-", f));
    RL_CHECK(!parseOptionValue("1e39", f)); // out of float range: std::stof throws
    RL_CHECK(f == 9.0f);

    // Locale independence: a game that switches LC_NUMERIC to a decimal-comma locale must not turn
    // "0.5" into 0 (upstream std::stof would). Only checked when such a locale is installed.
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous ? previous : "C";
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") || std::setlocale(LC_NUMERIC, "de_DE")) {
        RL_CHECK(parseOptionValue("0.5", f) && f == 0.5f);
        RL_CHECK_STR(formatOptionValue(0.5f), "0.5");
    }
    std::setlocale(LC_NUMERIC, saved.c_str());
}

void testVectorParsing() {
    Vec2f v2;
    RL_CHECK(parseOptionValue("1.5, 2.5", v2) && v2 == Vec2f(1.5f, 2.5f));
    Vec3f v3;
    RL_CHECK(parseOptionValue("1.0, 2.0, 3.0", v3) && v3 == Vec3f(1.0f, 2.0f, 3.0f));
    Vec4f v4;
    RL_CHECK(parseOptionValue("1.0, 2.0, 3.0, 4.0", v4) && v4 == Vec4f(1.0f, 2.0f, 3.0f, 4.0f));
    Vec2i v2i;
    RL_CHECK(parseOptionValue("100, 200", v2i) && v2i == Vec2i(100, 200));
    RL_CHECK(parseOptionValue("1,2", v2i) && v2i == Vec2i(1, 2)); // no spaces needed

    // Scalar promotion (test_configScalarToVectorPromotion part 1).
    RL_CHECK(parseOptionValue("1.5", v2) && v2 == Vec2f(1.5f));
    RL_CHECK(parseOptionValue("42", v2i) && v2i == Vec2i(42));
    RL_CHECK(parseOptionValue("2.5", v3) && v3 == Vec3f(2.5f));
    RL_CHECK(parseOptionValue("3.0", v4) && v4 == Vec4f(3.0f));
    RL_CHECK(parseOptionValue("4,", v3) && v3 == Vec3f(4.0f)); // no trailing empty piece: promoted

    // Part 2: a partial vector fails, and what was parsed before the failure is kept (upstream).
    Vec3f partial(9.0f, 9.0f, 9.0f);
    RL_CHECK(!parseOptionValue("2.5, 2.5", partial));
    RL_CHECK(partial == Vec3f(2.5f, 2.5f, 9.0f));
    Vec4f partial4(9.0f);
    RL_CHECK(!parseOptionValue("2.5, 2.5", partial4));
    RL_CHECK(!parseOptionValue("1, 2, x, 4", partial4));
    RL_CHECK(partial4 == Vec4f(1.0f, 2.0f, 9.0f, 9.0f));

    // Extra components are ignored.
    RL_CHECK(parseOptionValue("1, 2, 3", v2) && v2 == Vec2f(1.0f, 2.0f));
    // Nothing to parse.
    RL_CHECK(!parseOptionValue("", v3));
    RL_CHECK(!parseOptionValue("x", v3));
    RL_CHECK(!parseOptionValue("1,,3", v3)); // empty middle component
}

void testStringAndListParsing() {
    std::string s = "old";
    RL_CHECK(parseOptionValue("Hello World", s) && s == "Hello World");
    RL_CHECK(!parseOptionValue("", s) && s == "Hello World");

    std::vector<std::string> list;
    RL_CHECK(parseOptionValue("0x1234, 0x5678, 0xABCD", list) && list.size() == 3);
    RL_CHECK(list.size() == 3 && list[1] == " 0x5678"); // pieces are not trimmed
    RL_CHECK(splitConfigList("a,,b").size() == 3);
    RL_CHECK(splitConfigList("a,").size() == 1);
    RL_CHECK(splitConfigList("").empty());
    RL_CHECK(splitConfigList(",").size() == 1 && splitConfigList(",")[0].empty());
}

// ----------------------------------------------------------------------------
// Hash parsing and HashSetLayer (hash-set syntax)
// ----------------------------------------------------------------------------

void testHashParsing() {
    Hash64 h = 0;
    RL_CHECK(parseHashValue("0x8DD6F568BD126398", h) && h == 0x8DD6F568BD126398ull);
    RL_CHECK(parseHashValue("8dd6f568bd126398", h) && h == 0x8DD6F568BD126398ull); // prefix optional
    RL_CHECK(parseHashValue("  0X1a", h) && h == 0x1Aull);
    RL_CHECK(parseHashValue("0x12zz", h) && h == 0x12ull); // trailing text ignored
    RL_CHECK(parseHashValue("0x", h) && h == 0ull);        // "0", then 'x' stops (strtoull)
    RL_CHECK(parseHashValue("-0x1", h) && h == 0xFFFFFFFFFFFFFFFFull); // stoull negates
    RL_CHECK(parseHashValue("0xFFFFFFFFFFFFFFFF", h) && h == 0xFFFFFFFFFFFFFFFFull);
    RL_CHECK(!parseHashValue("0x1FFFFFFFFFFFFFFFF", h)); // 17 digits: out of range
    RL_CHECK(!parseHashValue("zz", h));
    RL_CHECK(!parseHashValue("", h));

    RL_CHECK_STR(formatHash(0x1Aull), "0x000000000000001A");
    RL_CHECK_STR(formatHash(0x8DD6F568BD126398ull), "0x8DD6F568BD126398");

    HashVector vector;
    std::vector<std::string> errors;
    RL_CHECK(parseHashVector(splitConfigList("0x3, 0x1, 0x3, xyz, 0x2"), vector, &errors) == 1);
    RL_CHECK((vector == HashVector{3, 1, 3, 2})); // order and duplicates kept
    RL_CHECK(errors.size() == 1);
    RL_CHECK_STR(formatOptionValue(vector),
                 "0x0000000000000003, 0x0000000000000001, 0x0000000000000003, 0x0000000000000002");
}

void testHashSetSyntax() {
    HashSetLayer set;
    std::vector<std::string> errors;
    const std::size_t failures = set.parseFromStrings(
        splitConfigList("0x3333333333333333, -0x2222222222222222 ,, 0x1111111111111111,  ,nothex, -0xAB"), &errors);
    RL_CHECK(failures == 1);
    RL_CHECK(errors.size() == 1);
    RL_CHECK(set.hasPositive(0x3333333333333333ull));
    RL_CHECK(set.hasPositive(0x1111111111111111ull));
    RL_CHECK(set.hasNegative(0x2222222222222222ull));
    RL_CHECK(set.hasNegative(0xABull));
    RL_CHECK(set.size() == 2 && set.negativeSize() == 2);
    // Canonical form: sorted positives, then sorted negatives, 16 upper-case digits.
    RL_CHECK_STR(set.toString(),
                 "0x1111111111111111, 0x3333333333333333, -0x00000000000000AB, -0x2222222222222222");

    // A value that is already canonical formats back byte for byte.
    const std::string canonical = "0x0000000000000001, 0x8DD6F568BD126398, -0xEEF8EFD4B8A1B2A5";
    HashSetLayer round;
    RL_CHECK(round.parseFromStrings(splitConfigList(canonical)) == 0);
    RL_CHECK_STR(round.toString(), canonical);
    RL_CHECK_STR(HashSetLayer().toString(), "");
}

void testHashSetLayerDirect() {
    // Port of test_hashSetLayerDirect.
    HashSetLayer layer1;
    const Hash64 h1 = 0x1000000000000001ull, h2 = 0x2000000000000002ull, h3 = 0x3000000000000003ull,
                 h4 = 0x4000000000000004ull;
    layer1.add(h1);
    layer1.add(h2);
    RL_CHECK(layer1.hasPositive(h1) && layer1.hasPositive(h2) && !layer1.hasNegative(h1));
    layer1.remove(h3);
    RL_CHECK(!layer1.hasPositive(h3) && layer1.hasNegative(h3));
    RL_CHECK(layer1.count(h1) == 1 && layer1.count(h3) == 0 && layer1.count(h4) == 0);
    layer1.add(h3);
    RL_CHECK(layer1.hasPositive(h3) && !layer1.hasNegative(h3));
    layer1.clear(h1);
    RL_CHECK(!layer1.hasPositive(h1) && !layer1.hasNegative(h1));

    HashSetLayer layer2;
    layer2.parseFromStrings({"0x1111111111111111", "0x2222222222222222", "-0x3333333333333333"});
    RL_CHECK(layer2.hasPositive(0x1111111111111111ull) && layer2.hasPositive(0x2222222222222222ull));
    RL_CHECK(layer2.hasNegative(0x3333333333333333ull));
    const std::string serialized = layer2.toString();
    RL_CHECK(serialized.find("0x1111111111111111") != std::string::npos);
    RL_CHECK(serialized.find("-0x3333333333333333") != std::string::npos);

    HashSetLayer base;
    base.add(0xAAAAAAAAAAAAAAAAull);
    base.add(0xBBBBBBBBBBBBBBBBull);
    HashSetLayer overrideLayer;
    overrideLayer.add(0xCCCCCCCCCCCCCCCCull);
    overrideLayer.remove(0xAAAAAAAAAAAAAAAAull);
    overrideLayer.mergeFrom(base);
    RL_CHECK(!overrideLayer.hasPositive(0xAAAAAAAAAAAAAAAAull) && overrideLayer.hasNegative(0xAAAAAAAAAAAAAAAAull));
    RL_CHECK(overrideLayer.hasPositive(0xBBBBBBBBBBBBBBBBull));
    RL_CHECK(overrideLayer.hasPositive(0xCCCCCCCCCCCCCCCCull));

    // Deltas used by export and the UI.
    HashSetLayer saved;
    saved.parseFromStrings({"0x1", "0x2", "-0x5"});
    HashSetLayer current;
    current.parseFromStrings({"0x2", "0x3", "-0x4", "-0x5"});
    const HashSetLayer added = current.computeAddedOpinions(saved);
    RL_CHECK(added.hasPositive(0x3) && added.hasNegative(0x4) && added.size() == 1 && added.negativeSize() == 1);
    RL_CHECK_STR(current.diffToString(saved), "+0x0000000000000003, ~0x0000000000000001, +-0x0000000000000004");

    // assignPositives (setDeferred of a whole set) keeps negatives but never both opinions.
    HashSetLayer assigned;
    assigned.parseFromStrings({"0x1", "-0x2", "-0x3"});
    assigned.assignPositives(HashSet{0x3, 0x4});
    RL_CHECK(!assigned.hasPositive(0x1) && assigned.hasPositive(0x3) && assigned.hasPositive(0x4));
    RL_CHECK(assigned.hasNegative(0x2) && !assigned.hasNegative(0x3));
}

// ----------------------------------------------------------------------------
// Formatting
// ----------------------------------------------------------------------------

void testFormatting() {
    RL_CHECK_STR(formatOptionValue(true), "True");
    RL_CHECK_STR(formatOptionValue(false), "False");
    RL_CHECK_STR(formatOptionValue(std::int32_t{-42}), "-42");
    // std::ostream << float in the "C" locale (printf %g, 6 significant digits).
    RL_CHECK_STR(formatOptionValue(1.5f), "1.5");
    RL_CHECK_STR(formatOptionValue(0.1f), "0.1");
    RL_CHECK_STR(formatOptionValue(3.14159f), "3.14159");
    RL_CHECK_STR(formatOptionValue(2.71828f), "2.71828");
    RL_CHECK_STR(formatOptionValue(1.23456789f), "1.23457");
    RL_CHECK_STR(formatOptionValue(1e-5f), "1e-05");
    RL_CHECK_STR(formatOptionValue(100000.0f), "100000");
    RL_CHECK_STR(formatOptionValue(1e6f), "1e+06");
    RL_CHECK_STR(formatOptionValue(0.0f), "0");
    RL_CHECK_STR(formatOptionValue(-0.0f), "-0");
    RL_CHECK_STR(formatOptionValue(Vec2f(1.5f, 2.5f)), "1.5, 2.5");
    RL_CHECK_STR(formatOptionValue(Vec3f(7.0f, 8.0f, 9.0f)), "7, 8, 9");
    RL_CHECK_STR(formatOptionValue(Vec4f(1.0f, 2.0f, 3.0f, 4.0f)), "1, 2, 3, 4");
    RL_CHECK_STR(formatOptionValue(Vec2i(100, 200)), "100, 200");
    RL_CHECK_STR(formatOptionValue(OptionValue(std::string("x y"))), "x y");

    // Formatting then parsing returns the same float for %g-exact values, and formatting is a fixed
    // point after one round (what the byte-stable save relies on).
    for (float v : {0.1f, 1.23456789f, 3.0e-7f, 12345678.0f, -0.333333f}) {
        float parsed = 0.0f;
        RL_CHECK(parseOptionValue(formatOptionValue(v), parsed));
        float reparsed = 0.0f;
        RL_CHECK(parseOptionValue(formatOptionValue(parsed), reparsed));
        RL_CHECK_STR(formatOptionValue(parsed), formatOptionValue(reparsed));
    }
}

// ----------------------------------------------------------------------------
// .conf parsing
// ----------------------------------------------------------------------------

void testConfSyntax() {
    const std::string text =
        "# comment\n"
        "\n"
        "   \t\n"
        "rtx.a = 1\n"
        "rtx.b=2\n"
        "  rtx.c   =   three words  \n"
        "rtx.d = \"  quoted  \"\n"
        "rtx.e = a\"b\"c\n"
        "rtx.f = x = y # not a comment\n"
        "rtx.crlf = True\r\n"
        "rtx.empty =\n"
        "d3d9.maxFrameRate = 60\n"
        "rtx.last = end";
    std::vector<ConfigDiagnostic> diagnostics;
    const OptionConfig config = OptionConfig::parse(text, exe("Game.exe"), &diagnostics);
    RL_CHECK(diagnostics.empty());
    RL_CHECK_STR(config.get<std::string>("rtx.a"), "1");
    RL_CHECK_STR(config.get<std::string>("rtx.b"), "2");
    RL_CHECK_STR(config.get<std::string>("rtx.c"), "three words  "); // trailing spaces kept
    RL_CHECK_STR(config.get<std::string>("rtx.d"), "  quoted  ");
    RL_CHECK_STR(config.get<std::string>("rtx.e"), "abc");
    RL_CHECK_STR(config.get<std::string>("rtx.f"), "x = y # not a comment");
    RL_CHECK(config.get<bool>("rtx.crlf", false)); // "\r\n" handled like a Windows text stream
    RL_CHECK(!config.contains("rtx.empty"));       // empty value = not set
    RL_CHECK(config.entries().count("rtx.empty") == 1);
    RL_CHECK(config.get<std::int32_t>("d3d9.maxFrameRate", 0) == 60);
    RL_CHECK_STR(config.get<std::string>("rtx.last"), "end"); // no final newline
    RL_CHECK(!config.get<bool>("rtx.c", false));
}

void testConfDiagnostics() {
    const std::string text =
        "rtx.ok = 1\n"                  // 1
        "rtx.bad-key = 1\n"             // 2 invalid character
        "rtx.noequals 5\n"              // 3 missing '='
        "just some words\n"             // 4 missing '='
        "= 5\n"                         // 5 empty key
        "rtx.quote = \"unterminated\n"  // 6 unterminated quote (value still set)
        "rtx.ok = 2\n"                  // 7 duplicate: last wins
        "[NoClose\n"                    // 8 section without ']'
        "rtx.hidden = 1\n";             // 9 inside a section that matches nothing
    std::vector<ConfigDiagnostic> diagnostics;
    const OptionConfig config = OptionConfig::parse(text, exe("Game.exe"), &diagnostics);
    RL_CHECK(hasDiagnostic(diagnostics, 2, "invalid character '-'"));
    RL_CHECK(hasDiagnostic(diagnostics, 3, "missing '='"));
    RL_CHECK(hasDiagnostic(diagnostics, 4, "missing '='"));
    RL_CHECK(hasDiagnostic(diagnostics, 5, "empty key"));
    RL_CHECK(hasDiagnostic(diagnostics, 6, "unterminated quote"));
    RL_CHECK(hasDiagnostic(diagnostics, 7, "overrides line 1"));
    RL_CHECK(hasDiagnostic(diagnostics, 8, "without ']'"));
    RL_CHECK(diagnostics.size() == 7);
    RL_CHECK(config.get<std::int32_t>("rtx.ok", 0) == 2);
    RL_CHECK(!config.contains("rtx.bad"));
    RL_CHECK(!config.contains("rtx.noequals"));
    RL_CHECK_STR(config.get<std::string>("rtx.quote"), "unterminated");
    RL_CHECK(!config.contains("rtx.hidden"));
    RL_CHECK(config.size() == 2);

    // A UTF-8 byte order mark hides the first key (upstream parity) and is reported.
    diagnostics.clear();
    const OptionConfig bom = OptionConfig::parse("\xEF\xBB\xBFrtx.first = 1\nrtx.second = 2\n", exe("Game.exe"),
                                                 &diagnostics);
    RL_CHECK(!bom.contains("rtx.first") && bom.contains("rtx.second"));
    RL_CHECK(hasDiagnostic(diagnostics, 1, "byte order mark"));
}

void testConfSections() {
    const std::string text =
        "rtx.global = 1\n"
        "[Game.exe]\n"
        "rtx.game = 1\n"
        "[Other.exe]\n"
        "rtx.other = 1\n"
        "rtx.global = 2\n"
        "[Game.exe]\n"
        "rtx.game2 = 1\n";
    const OptionConfig game = OptionConfig::parse(text, exe("Game.exe"));
    RL_CHECK(game.get<std::int32_t>("rtx.global", 0) == 1);
    RL_CHECK(game.contains("rtx.game") && game.contains("rtx.game2") && !game.contains("rtx.other"));
    const OptionConfig other = OptionConfig::parse(text, exe("Other.exe"));
    RL_CHECK(other.get<std::int32_t>("rtx.global", 0) == 2);
    RL_CHECK(other.contains("rtx.other") && !other.contains("rtx.game"));
    // Section names are compared exactly (no trimming, case-sensitive), as upstream.
    RL_CHECK(!OptionConfig::parse("[ Game.exe ]\nrtx.x = 1\n", exe("Game.exe")).contains("rtx.x"));
    RL_CHECK(!OptionConfig::parse("[game.exe]\nrtx.x = 1\n", exe("Game.exe")).contains("rtx.x"));
#if defined(__linux__) || defined(_WIN32)
    // Without an explicit name, sections match the running executable's file name.
    const std::string self = currentExecutableName();
    RL_CHECK(!self.empty() && self.find_first_of("/\\") == std::string::npos);
    RL_CHECK(OptionConfig::parse("[" + self + "]\nrtx.x = 1\n").contains("rtx.x"));
#endif
}

void testConfigGetSemantics() {
    OptionConfig config;
    config.set("rtx.test.v3", "1, 2");
    config.setValue("rtx.test.serializeBool", true);
    config.setValue("rtx.test.serializeInt", std::int32_t{12345});
    config.setValue("rtx.test.serializeFloat", 3.14159f);
    config.setValue("rtx.test.serializeString", std::string("Hello World"));
    config.setValue("rtx.test.serializeVector2", Vec2f(1.5f, 2.5f));
    config.setValue("rtx.test.serializeVector3", Vec3f(1.0f, 2.0f, 3.0f));
    config.setValue("rtx.test.serializeVector4", Vec4f(1.0f, 2.0f, 3.0f, 4.0f));
    config.setValue("rtx.test.serializeVector2i", Vec2i(100, 200));

    // test_configSerialization part 1.
    RL_CHECK(config.get<bool>("rtx.test.serializeBool", false));
    RL_CHECK(config.get<std::int32_t>("rtx.test.serializeInt", 0) == 12345);
    RL_CHECK_NEAR(config.get<float>("rtx.test.serializeFloat", 0.0f), 3.14159f, 0.00001f);
    RL_CHECK_STR(config.get<std::string>("rtx.test.serializeString", ""), "Hello World");
    RL_CHECK(config.get<Vec2f>("rtx.test.serializeVector2") == Vec2f(1.5f, 2.5f));
    RL_CHECK(config.get<Vec3f>("rtx.test.serializeVector3") == Vec3f(1.0f, 2.0f, 3.0f));
    RL_CHECK(config.get<Vec4f>("rtx.test.serializeVector4") == Vec4f(1.0f, 2.0f, 3.0f, 4.0f));
    RL_CHECK(config.get<Vec2i>("rtx.test.serializeVector2i") == Vec2i(100, 200));

    // Missing key: fallback. Failed parse: the fallback with whatever parsed first (upstream).
    RL_CHECK(config.get<std::int32_t>("rtx.missing", 17) == 17);
    RL_CHECK(config.get<Vec3f>("rtx.test.v3", Vec3f(9.0f)) == Vec3f(1.0f, 2.0f, 9.0f));

    // test_configScalarToVectorPromotion part 3.
    config.set("rtx.test.promoteV2", "1.25");
    config.set("rtx.test.promoteV2i", "99");
    RL_CHECK(config.get<Vec2f>("rtx.test.promoteV2") == Vec2f(1.25f));
    RL_CHECK(config.get<Vec2i>("rtx.test.promoteV2i") == Vec2i(99));

    // Environment override on top of the file value (Config::getOption envVarName).
    setEnvironmentVariable("RL_OPTIONS_TEST_GET_ENV", "77");
    RL_CHECK(config.get<std::int32_t>("rtx.test.serializeInt", 0, "RL_OPTIONS_TEST_GET_ENV") == 77);
    setEnvironmentVariable("RL_OPTIONS_TEST_GET_ENV", "");
    RL_CHECK(config.get<std::int32_t>("rtx.test.serializeInt", 0, "RL_OPTIONS_TEST_GET_ENV") == 12345);

    // merge(): the argument wins.
    OptionConfig other;
    other.set("rtx.test.serializeInt", "1");
    other.set("rtx.test.onlyOther", "x");
    config.merge(other);
    RL_CHECK(config.get<std::int32_t>("rtx.test.serializeInt", 0) == 1);
    RL_CHECK(config.contains("rtx.test.onlyOther"));
}

// ----------------------------------------------------------------------------
// Serialization (byte-stable)
// ----------------------------------------------------------------------------

void testSerializeCanonicalRoundTrip() {
    // A canonical file: sorted keys, "key = value", '\n' endings, Remix value formatting.
    const std::string canonical =
        "rtx.a.bool = True\n"
        "rtx.b.int = -42\n"
        "rtx.c.float = 0.1\n"
        "rtx.d.vec3 = 1, 2.5, -3\n"
        "rtx.e.hashes = 0x0000000000000001, 0x8DD6F568BD126398, -0xEEF8EFD4B8A1B2A5\n"
        "rtx.f.string = two words\n"
        "rtx.g.lead = \"  leading spaces\"\n"
        "rtx.h.trail = trailing spaces  \n";
    const OptionConfig config = OptionConfig::parse(canonical, exe("Game.exe"));
    RL_CHECK_STR(config.serialize(defaultSaveKeyFilters()), canonical);

    // File round trip.
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_options_serialize");
    const std::string path = (dir / "rtx.conf").string();
    {
        std::ofstream out(path, std::ios::binary);
        out << canonical;
    }
    bool found = false;
    std::vector<ConfigDiagnostic> diagnostics;
    const OptionConfig loaded = OptionConfig::loadFile(path, exe("Game.exe"), &diagnostics, &found);
    RL_CHECK(found && diagnostics.empty());
    const std::string rewritten = (dir / "rtx_out.conf").string();
    RL_CHECK(loaded.saveFile(rewritten, defaultSaveKeyFilters()));
    RL_CHECK_STR(readFile(rewritten), canonical);

    bool missingFound = true;
    RL_CHECK(OptionConfig::loadFile((dir / "missing.conf").string(), exe("Game.exe"), nullptr, &missingFound).empty());
    RL_CHECK(!missingFound);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testSerializeRules() {
    OptionConfig config;
    config.set("rtx.z", "1");
    config.set("rtx.a", "2");
    config.set("rtx.empty", "");
    config.set("d3d9.presentInterval", "1");
    config.set("relight.native", "3");
    config.set("xrtx.substring", "4"); // "rtx." matches anywhere in the key, as upstream's find()
    // Keys sorted; empty values skipped; filter by substring.
    RL_CHECK_STR(config.serialize(defaultSaveKeyFilters()),
                 "relight.native = 3\nrtx.a = 2\nrtx.z = 1\nxrtx.substring = 4\n");
    RL_CHECK_STR(config.serialize(),
                 "d3d9.presentInterval = 1\nrelight.native = 3\nrtx.a = 2\nrtx.z = 1\nxrtx.substring = 4\n");
    RL_CHECK_STR(config.serialize({"d3d9."}), "d3d9.presentInterval = 1\n");

    // Messy input is normalized once, then stable.
    const std::string messy =
        "  rtx.b=2\r\n"
        "# comment\n"
        "rtx.a =    \"  x\"\n"
        "rtx.b = 3\n"
        "rtx.c = \"\"\n";
    const std::string once = OptionConfig::parse(messy, exe("Game.exe")).serialize(defaultSaveKeyFilters());
    RL_CHECK_STR(once, "rtx.a = \"  x\"\nrtx.b = 3\n");
    const std::string twice = OptionConfig::parse(once, exe("Game.exe")).serialize(defaultSaveKeyFilters());
    RL_CHECK_STR(twice, once);
}

} // namespace

void runConfigTests() {
    static const TestCase kCases[] = {
        {"boolParsing", testBoolParsing},
        {"intParsing", testIntParsing},
        {"floatParsing", testFloatParsing},
        {"vectorParsing", testVectorParsing},
        {"stringAndListParsing", testStringAndListParsing},
        {"hashParsing", testHashParsing},
        {"hashSetSyntax", testHashSetSyntax},
        {"hashSetLayerDirect", testHashSetLayerDirect},
        {"formatting", testFormatting},
        {"confSyntax", testConfSyntax},
        {"confDiagnostics", testConfDiagnostics},
        {"confSections", testConfSections},
        {"configGetSemantics", testConfigGetSemantics},
        {"serializeCanonicalRoundTrip", testSerializeCanonicalRoundTrip},
        {"serializeRules", testSerializeRules},
    };
    runSuite("config", kCases);
}

} // namespace rl_options_test
