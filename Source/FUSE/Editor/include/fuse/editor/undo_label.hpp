#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fuse::editor {

/// Trim padding and clamp an undo-command label to `maxChars`.
/// Leading and trailing ASCII space and tab are removed. When the trimmed text
/// exceeds `maxChars` and `maxChars >= 3`, the result is `maxChars` long with
/// the last three characters replaced by "...".
[[nodiscard]] inline std::string trim_undo_label(std::string_view text, std::size_t maxChars) {
    if (maxChars == 0) {
        return {};
    }

    constexpr std::string_view kPadding = " \t";
    const std::size_t begin = text.find_first_not_of(kPadding);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(kPadding);
    const std::string_view trimmed = text.substr(begin, end - begin + 1);
    if (trimmed.size() <= maxChars) {
        return std::string(trimmed);
    }
    if (maxChars < 3) {
        return std::string(trimmed.substr(0, maxChars));
    }

    std::string result(trimmed.substr(0, maxChars));
    result.replace(maxChars - 3, 3, "...");
    return result;
}

} // namespace fuse::editor
