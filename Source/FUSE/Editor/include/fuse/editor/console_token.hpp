#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fuse::editor {

/// Split a console line into tokens.
///
/// ASCII space (U+0020) separates tokens. Leading, trailing, and repeated spaces
/// are not tokens, so an empty or all-space line writes zero tokens. A double
/// quote opens or closes a group: spaces inside the group stay in the token, and
/// the quote characters themselves are omitted. Text tight against a group stays
/// in the same token (`say"hi"` is `sayhi`). A backslash is copied literally.
/// An unclosed quote still emits the token collected so far.
///
/// At most `capacity` tokens are written to `out`. `*count` is how many were
/// written; further tokens are dropped. A null `out` or a null `count` returns
/// without writing.
inline void tokenize_console(std::string_view line, std::string* out, int capacity, int* count) {
    if (out == nullptr || count == nullptr) {
        return;
    }
    if (capacity <= 0) {
        *count = 0;
        return;
    }

    int written = 0;
    std::string token;
    bool inToken = false;
    bool inQuotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inQuotes) {
            if (ch == '"') {
                inQuotes = false;
            } else {
                token.push_back(ch);
            }
            continue;
        }
        if (ch == '"') {
            inQuotes = true;
            inToken = true;
            continue;
        }
        if (ch == ' ') {
            if (!inToken) {
                continue;
            }
            out[written] = std::move(token);
            token.clear();
            ++written;
            inToken = false;
            if (written >= capacity) {
                *count = written;
                return;
            }
            continue;
        }
        token.push_back(ch);
        inToken = true;
    }

    if (inToken && written < capacity) {
        out[written] = std::move(token);
        ++written;
    }
    *count = written;
}

} // namespace fuse::editor
