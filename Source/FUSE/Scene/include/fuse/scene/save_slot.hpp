#pragma once

#include <string>
#include <string_view>

namespace fuse::scene {

/// Save-slot id: ASCII letters and digits, '_' and '-'.
/// Spaces become '_'; other characters are dropped; letters are lowercased.
/// Repeated '_' collapse to one, then leading and trailing '_' are removed.
/// An empty result becomes "slot". The result is at most 32 characters.
[[nodiscard]] inline std::string sanitize_save_slot(std::string_view name) {
    constexpr std::size_t kMaxLen = 32;

    std::string out;
    out.reserve(name.size());
    for (const char raw : name) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        char mapped = 0;
        if (ch == ' ') {
            mapped = '_';
        } else if (ch >= 'A' && ch <= 'Z') {
            mapped = static_cast<char>(ch - 'A' + 'a');
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
            mapped = static_cast<char>(ch);
        } else {
            continue;
        }
        if (mapped == '_' && !out.empty() && out.back() == '_') {
            continue;
        }
        out.push_back(mapped);
    }

    std::size_t begin = 0;
    while (begin < out.size() && out[begin] == '_') {
        ++begin;
    }
    std::size_t end = out.size();
    while (end > begin && out[end - 1] == '_') {
        --end;
    }
    if (begin > 0 || end < out.size()) {
        out = out.substr(begin, end - begin);
    }
    if (out.empty()) {
        return "slot";
    }
    if (out.size() > kMaxLen) {
        out.resize(kMaxLen);
    }
    return out;
}

} // namespace fuse::scene
