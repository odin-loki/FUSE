#pragma once

namespace fuse::adventure {

/// Wrapped line count for examine text.
/// Non-positive `columns` or `charCount` yields 0. Otherwise the ceiling of `charCount / columns`.
[[nodiscard]] inline int examine_line_count(int charCount, int columns) {
    if (columns <= 0 || charCount <= 0) {
        return 0;
    }
    const long long count = charCount;
    const long long width = columns;
    return static_cast<int>((count + width - 1) / width);
}

/// True when the wrapped text fits in `maxLines`, or when there is nothing to show.
/// A non-positive `maxLines` rejects any text that produces lines.
[[nodiscard]] inline bool examine_fits(int charCount, int columns, int maxLines) {
    if (charCount <= 0) {
        return true;
    }
    const int lines = examine_line_count(charCount, columns);
    return lines > 0 && lines <= maxLines;
}

} // namespace fuse::adventure
