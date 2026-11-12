// Internal: number / separator scanning shared by SVG path data (path.cpp) and SVG documents (svg.cpp).
#pragma once

#include "fuse/text/from_chars.hpp"
#include "fuse/ui/types.hpp"

namespace fuse::ui::detail {

inline bool svgSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline bool svgDigit(char c) { return c >= '0' && c <= '9'; }

/// Skips whitespace and at most one comma.
inline void svgSkipSep(const char*& p, const char* end) {
    while (p < end && svgSpace(*p)) ++p;
    if (p < end && *p == ',') {
        ++p;
        while (p < end && svgSpace(*p)) ++p;
    }
}

/// Parses one SVG number ([+-] digits [. digits] [e [+-] digits]) at `p`; false (p unchanged) when there is none or
/// it is not finite.
inline bool svgNumber(const char*& p, const char* end, f32& out) {
    const char* s = p;
    bool neg = false;
    if (s < end && (*s == '+' || *s == '-')) neg = *s++ == '-';
    const char* digits = s;
    const char* q = s;
    while (q < end && svgDigit(*q)) ++q;
    const bool intPart = q > digits;
    bool fracPart = false;
    if (q < end && *q == '.') {
        const char* f = q + 1;
        while (f < end && svgDigit(*f)) ++f;
        fracPart = f > q + 1;
        if (intPart || fracPart) q = f;
    }
    if (!intPart && !fracPart) return false;
    if (q < end && (*q == 'e' || *q == 'E')) { // an exponent needs digits ("1em" is a unit, not an exponent)
        const char* e = q + 1;
        if (e < end && (*e == '+' || *e == '-')) ++e;
        if (e < end && svgDigit(*e)) {
            while (e < end && svgDigit(*e)) ++e;
            q = e;
        }
    }
    char buf[64];
    const std::size_t n = std::size_t(q - digits);
    if (n == 0 || n + 2 > sizeof buf) return false;
    std::size_t k = 0;
    if (*digits == '.') buf[k++] = '0'; // ".5" -> "0.5" (strtod-based fallbacks want a leading digit)
    for (std::size_t i = 0; i < n; ++i) buf[k++] = digits[i];
    double v = 0.0;
    const auto r = fuse::text::fromChars(buf, buf + k, v);
    if (r.ec != std::errc{}) return false;
    if (!(v > -1e30 && v < 1e30)) return false; // also rejects NaN
    out = f32(neg ? -v : v);
    p = q;
    return true;
}

} // namespace fuse::ui::detail
