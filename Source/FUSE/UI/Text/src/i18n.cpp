// Pseudo-localisation (fuse/text/i18n.hpp). The plural and number functions of that header are implemented in
// fuse_ui_l10n_lib (L10n/src/text_i18n.cpp) on the l10n tables; fuse_text links it.
#include "fuse/text/i18n.hpp"

#include "fuse/text/text.hpp"

#include <cmath>

namespace fuse::text {

// ---- Pseudo-localisation -------------------------------------------------------------------------------------------
namespace {
// Accented look-alikes from Latin-1 / Latin Extended-A (covered by the bundled UI font), indexed by letter.
constexpr char32_t kPseudoLower[26] = {0x00E1, 'b',    0x00E7, 0x00F0, 0x00E9, 0x0192, 0x011D, 0x0125, 0x00ED,
                                       0x0135, 0x0137, 0x013C, 'm',    0x00F1, 0x00F6, 'p',    'q',    0x0155,
                                       0x0161, 0x0163, 0x00FC, 'v',    0x0175, 'x',    0x00FD, 0x017E};
constexpr char32_t kPseudoUpper[26] = {0x00C5, 'B',    0x00C7, 0x00D0, 0x00C9, 'F',    0x011C, 0x0124, 0x00CE,
                                       0x0134, 0x0136, 0x013B, 'M',    0x00D1, 0x00D6, 'P',    'Q',    0x0154,
                                       0x0160, 0x0162, 0x00DB, 'V',    0x0174, 'X',    0x00DD, 0x017D};
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
/// Length of the verbatim span starting at `i` (placeholder, printf conversion, tag, entity); 0 when none.
std::size_t protectedSpan(std::string_view s, std::size_t i) {
    const char c = s[i];
    if (c == '{') {
        int depth = 0;
        for (std::size_t k = i; k < s.size(); ++k) {
            if (s[k] == '{') ++depth;
            else if (s[k] == '}' && --depth == 0) return k - i + 1;
        }
        return 0;
    }
    if (c == '<') {
        const std::size_t e = s.find('>', i);
        if (e != std::string_view::npos && e > i + 1 && (isAlpha(s[i + 1]) || s[i + 1] == '/')) return e - i + 1;
        return 0;
    }
    if (c == '&') {
        std::size_t k = i + 1;
        while (k < s.size() && (isAlpha(s[k]) || isDigit(s[k]) || s[k] == '#') && k - i < 10) ++k;
        return (k < s.size() && s[k] == ';' && k > i + 1) ? k - i + 1 : 0;
    }
    if (c == '%' && i + 1 < s.size()) {
        if (s[i + 1] == '%') return 2;
        std::size_t k = i + 1;
        while (k < s.size() && (isDigit(s[k]) || s[k] == '$' || s[k] == '.' || s[k] == '-' || s[k] == '+' || s[k] == 'l' ||
                                s[k] == 'h' || s[k] == 'z'))
            ++k;
        if (k < s.size() && std::string_view("diuoxXfFeEgGscp@").find(s[k]) != std::string_view::npos) return k - i + 1;
        // Qt style %1 .. %99
        if (k > i + 1 && isDigit(s[k - 1])) return k - i;
        return 0;
    }
    return 0;
}
} // namespace

std::string pseudoLocalize(std::string_view s, const PseudoOptions& opt) {
    std::string out;
    out.reserve(s.size() * 2 + 8);
    if (opt.brackets) out.push_back('[');
    std::size_t letters = 0;
    for (std::size_t i = 0; i < s.size();) {
        if (const std::size_t span = protectedSpan(s, i)) {
            out.append(s.substr(i, span));
            i += span;
            continue;
        }
        const char c = s[i++];
        if (isAlpha(c)) {
            ++letters;
            if (opt.accents) {
                appendUtf8(out, (c >= 'a') ? kPseudoLower[c - 'a'] : kPseudoUpper[c - 'A']);
                continue;
            }
        }
        out.push_back(c);
    }
    if (opt.expansion > 0.f && letters > 0) {
        const std::size_t pad = std::size_t(std::ceil(float(letters) * opt.expansion));
        out.push_back(' ');
        for (std::size_t k = 0; k < pad; ++k) out += "\xC2\xB7"; // middle dots: visibly padding, never a word
    }
    if (opt.brackets) out.push_back(']');
    return out;
}

} // namespace fuse::text
