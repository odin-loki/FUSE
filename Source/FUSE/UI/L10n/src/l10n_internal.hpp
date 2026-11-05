// fuse_ui_l10n internals: locale tag splitting.
#pragma once

#include <string>
#include <string_view>

namespace fuse::ui::l10n::detail {

/// "pt_BR" / "pt-br" -> lang "pt", region "br" (lower case; script subtags such as "zh-Hant" are skipped).
struct Tag {
    std::string lang, region;
};
inline Tag splitTag(std::string_view loc) {
    Tag t;
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

} // namespace fuse::ui::l10n::detail
