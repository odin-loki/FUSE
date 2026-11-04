// fuse_ui_l10n: localisation without ICU. CLDR plural rules (cardinal + ordinal) with the CLDR operands, ICU-style
// message formatting ({name}, plural / selectordinal / select, number, date), locale number and date formatting, and
// the "key=value" string-table file format shared by StringTable and fuse_ui_extract_strings.
//
// Locale tags are BCP 47-ish ("de", "de-CH", "pt_PT", "en-IN"); matching is case-insensitive, region first, then the
// language. Unknown languages get the CLDR root plural rule (other only) and English number / date data. Digits are
// always Latin (ar uses its latn number symbols).
#pragma once

#include <cstdint>
#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fuse::ui::l10n {

// ---- plural rules ----------------------------------------------------------------------------------------------
enum class Plural : std::uint8_t { Zero, One, Two, Few, Many, Other };
std::string_view pluralName(Plural p); ///< "zero" .. "other"
bool pluralFromName(std::string_view name, Plural& out);

/// CLDR plural operands of a decimal number: n absolute value, i integer digits, v visible fraction digit count,
/// f visible fraction digits, t f without trailing zeros. "1.50" -> n 1.5, i 1, v 2, f 50, t 5.
struct PluralOperands {
    double n = 0;
    std::uint64_t i = 0;
    std::uint32_t v = 0;
    std::uint64_t f = 0, t = 0;

    static PluralOperands fromDouble(double x); ///< shortest round-trip decimal (1.5 -> v 1; 2.0 -> v 0)
    static PluralOperands fromInt(std::int64_t x);
    static bool parse(std::string_view decimal, PluralOperands& out); ///< "-12.340" keeps v 3; false if malformed
};

Plural pluralCardinal(std::string_view locale, const PluralOperands& op);
Plural pluralOrdinal(std::string_view locale, const PluralOperands& op);
inline Plural pluralCardinal(std::string_view locale, double x) { return pluralCardinal(locale, PluralOperands::fromDouble(x)); }
inline Plural pluralOrdinal(std::string_view locale, double x) { return pluralOrdinal(locale, PluralOperands::fromDouble(x)); }

// ---- numbers and dates -----------------------------------------------------------------------------------------
struct NumberOptions {
    int minFraction = 0, maxFraction = 3;
    bool grouping = true;
};
/// Locale decimal: "1,234.5" (en), "1.234,5" (de), "1 234,5" (fr, narrow no-break space), "12,34,567" (hi, en-IN).
std::string formatNumber(std::string_view locale, double x, NumberOptions opt = {});
/// Locale percent of a fraction: 0.25 -> "25%" (en), "25 %" (de, no-break space), "%25" (tr).
std::string formatPercent(std::string_view locale, double fraction, int maxFraction = 0);
std::string_view decimalSeparator(std::string_view locale);
std::string_view groupingSeparator(std::string_view locale);

struct Date {
    int year = 1970, month = 1, day = 1; ///< proleptic Gregorian, month 1..12
};
enum class DateStyle : std::uint8_t { Short, Medium, Long };
/// CLDR gregorian date patterns: en Long "March 5, 2024", de Long "5. März 2024", ja Long "2024年3月5日".
std::string formatDate(std::string_view locale, Date d, DateStyle style = DateStyle::Medium);
std::string_view monthName(std::string_view locale, int month, bool abbreviated = false); ///< format context

// ---- messages --------------------------------------------------------------------------------------------------
/// A message argument: text, number or date.
using Value = std::variant<std::string, double, Date>;
struct Arg {
    std::string name;
    Value value;
    Arg(std::string_view n, std::string_view s) : name(n), value(std::string(s)) {}
    Arg(std::string_view n, const char* s) : name(n), value(std::string(s)) {}
    Arg(std::string_view n, const std::string& s) : name(n), value(s) {}
    Arg(std::string_view n, double x) : name(n), value(x) {}
    Arg(std::string_view n, int x) : name(n), value(double(x)) {}
    Arg(std::string_view n, long x) : name(n), value(double(x)) {}
    Arg(std::string_view n, long long x) : name(n), value(double(x)) {}
    Arg(std::string_view n, unsigned x) : name(n), value(double(x)) {}
    Arg(std::string_view n, unsigned long x) : name(n), value(double(x)) {}
    Arg(std::string_view n, unsigned long long x) : name(n), value(double(x)) {}
    Arg(std::string_view n, Date d) : name(n), value(d) {}
};
/// Named arguments; positional ones are named "0", "1", ...
class Args {
public:
    Args() = default;
    Args(std::initializer_list<Arg> a) : m_args(a) {}
    Args& add(Arg a) { m_args.push_back(std::move(a)); return *this; }
    const Value* find(std::string_view name) const;
    bool empty() const { return m_args.empty(); }

private:
    std::vector<Arg> m_args;
};

struct MessageError {
    std::size_t offset = 0; ///< byte offset into the pattern
    std::string message;
};

/// ICU MessageFormat (1) syntax:
///   {name}  {n, number[, integer|percent]}  {d, date[, short|medium|long]}
///   {n, plural, [offset:k] =0 {..} one {# item} other {# items}}   {n, selectordinal, one {#st} ..}
///   {g, select, male {..} female {..} other {..}}
/// '#' in a plural / selectordinal sub-message is the number minus the offset, formatted for the locale. Exact
/// matches (=k) compare the value before the offset. Apostrophes follow ICU: '' is a literal apostrophe, and a single
/// apostrophe quotes literal text only when it precedes {, } (or # inside a plural); otherwise it is literal. An
/// argument that is not given formats as "{name}".
std::expected<std::string, MessageError> formatMessage(std::string_view locale, std::string_view pattern,
                                                       const Args& args);
/// formatMessage, or the pattern itself when it does not parse.
std::string formatMessageOr(std::string_view locale, std::string_view pattern, const Args& args);

// ---- string-table files ----------------------------------------------------------------------------------------
// One "key=value" entry per line, '#' starts a comment line. Escapes: \\ \n \t, and in keys \= (a key ends at the
// first unescaped '=') and a leading \#. fuse_ui_extract_strings also writes the comment forms "#? key"
// (untranslated), "#~ key=value" (obsolete) and "#: location".
struct TableEntry {
    std::string key, value;
};
std::vector<TableEntry> parseTable(std::string_view text);
std::string escapeTableKey(std::string_view key);
std::string escapeTableValue(std::string_view value);
/// Splits one line "key=value" (no comment check) with unescaping; false without an unescaped '='.
bool parseTableLine(std::string_view line, std::string& key, std::string& value);
std::string unescapeTable(std::string_view s);

} // namespace fuse::ui::l10n
