// fuse_ui_l10n internals: locale tag splitting, shortest fixed-notation decimals.
#pragma once

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

namespace fuse::ui::l10n::detail {

/// std::to_chars(first, last, v, std::chars_format::fixed): the fewest digits that round-trip, never an exponent.
/// Apple's libc++ ties the floating-point std::to_chars to the OS version (macOS 13.3 / iOS 16.3), so there (and with
/// FUSE_TEXT_FROM_CHARS_FALLBACK, as in fuse/text/from_chars.hpp) the digits come from printf.
inline std::to_chars_result toCharsFixed(char* first, char* last, double v) {
#if defined(__cpp_lib_to_chars) && !defined(__APPLE__) && !defined(FUSE_TEXT_FROM_CHARS_FALLBACK)
    return std::to_chars(first, last, v, std::chars_format::fixed);
#else
    char sci[64], buf[400];
    int digits = 1;
    for (; digits <= 17; ++digits) { // fewest significant digits that read back as v
        std::snprintf(sci, sizeof sci, "%.*e", digits - 1, v);
        if (std::strtod(sci, nullptr) == v) break;
    }
    const char* e = std::strchr(sci, 'e');
    const int exp10 = e ? std::atoi(e + 1) : 0;
    const int frac = digits - 1 - exp10 > 0 ? digits - 1 - exp10 : 0;
    const int n = std::snprintf(buf, sizeof buf, "%.*f", frac, v);
    if (n < 0 || n >= int(sizeof buf) || n > last - first) return {last, std::errc::value_too_large};
    std::memcpy(first, buf, std::size_t(n));
    return {first + n, std::errc{}};
#endif
}

/// "pt_BR" / "pt-br" -> lang "pt", region "br" (lower case; script subtags such as "zh-Hant" are skipped; a POSIX
/// codeset / modifier suffix, "de_DE.UTF-8", "sr_RS@latin", is dropped).
struct Tag {
    std::string lang, region;
};
inline Tag splitTag(std::string_view loc) {
    Tag t;
    loc = loc.substr(0, loc.find_first_of(".@"));
    std::size_t k = 0;
    int part = 0;
    while (k <= loc.size()) {
        const std::size_t e = loc.find_first_of("-_", k);
        const std::string_view s = loc.substr(k, e == loc.npos ? loc.npos : e - k);
        std::string low;
        for (char c : s) low += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        if (part == 0) t.lang = low;
        else if (t.region.empty() && (low.size() == 2 || (low.size() == 3 && low[0] >= '0' && low[0] <= '9')))
            t.region = low;
        ++part;
        if (e == loc.npos) break;
        k = e + 1;
    }
    return t;
}

/// The number-symbol table of format.cpp, for fuse/text/i18n.hpp (text_i18n.cpp): one table for both APIs.
struct NumberRow {
    std::string_view decimal, group;
    int primary = 3, secondary = 3, minGrouping = 1;
};
std::size_t numberRowCount();
NumberRow numberRow(std::size_t index);
/// Row of `locale` (region entry, then the language, then English = 0).
std::size_t numberRowIndex(std::string_view locale);

} // namespace fuse::ui::l10n::detail
