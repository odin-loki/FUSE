#pragma once

#include <string_view>

namespace fuse::shipping_qt_detail {

[[nodiscard]] constexpr std::string_view trim_ascii(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] constexpr bool is_ident_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

[[nodiscard]] constexpr bool is_ident(std::string_view text) {
    for (const char c : text) {
        if (!is_ident_char(c)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool is_header_ext(std::string_view ext) {
    return ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx" || ext == ".inl";
}

[[nodiscard]] constexpr std::string_view header_stem(std::string_view segment) {
    const std::size_t dot = segment.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return segment;
    }
    if (!is_header_ext(segment.substr(dot))) {
        return segment;
    }
    return segment.substr(0, dot);
}

/// Path segment or header token: `Qt`/`qt` (`QtCore`, `QtGui`), a public Qt
/// header (`QWidget`, `QString`), or a lowercase Qt header (`qapplication.h`).
[[nodiscard]] constexpr bool is_qt_segment(std::string_view segment) {
    segment = trim_ascii(segment);
    if (segment.empty() || segment == "." || segment == "..") {
        return false;
    }

    const std::string_view stem = header_stem(segment);
    if (stem.size() >= 2 && (stem[0] == 'Q' || stem[0] == 'q') && stem[1] == 't' && is_ident(stem.substr(2))) {
        return true;
    }
    if (stem.size() >= 2 && stem[0] == 'Q' && stem[1] >= 'A' && stem[1] <= 'Z' && is_ident(stem.substr(2))) {
        return true;
    }

    // Qt's generated lowercase headers are `q` + lowercase name + `.h`.
    // `quat.hpp` is not one of them.
    if (segment.size() >= 4 && segment.ends_with(".h")) {
        const std::string_view base = segment.substr(0, segment.size() - 2);
        if (base.size() >= 2 && base[0] == 'q') {
            for (const char c : base) {
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool comment_only(std::string_view line) {
    if (line.empty() || line.starts_with("//")) {
        return true;
    }
    if (!line.starts_with("/*")) {
        return false;
    }
    const std::size_t end = line.find("*/");
    if (end == std::string_view::npos) {
        return true;
    }
    return comment_only(trim_ascii(line.substr(end + 2)));
}

} // namespace fuse::shipping_qt_detail

namespace fuse {

/// True when `line` is a shipping-banned Qt `#include`.
/// ASCII space and tab are trimmed first. The line must then start with
/// `#include`, and the included name must contain `Qt` or `qt` as a path
/// segment or header token (`QWidget`, `QtCore`, `QtGui`, `<QString>`,
/// `"qapplication.h"`). A comment-only line is false. Qt mentioned in a
/// string that is not an include is false.
[[nodiscard]] inline constexpr bool shipping_qt_include(std::string_view line) {
    line = shipping_qt_detail::trim_ascii(line);
    if (shipping_qt_detail::comment_only(line)) {
        return false;
    }
    constexpr std::string_view kInclude = "#include";
    if (!line.starts_with(kInclude)) {
        return false;
    }
    line.remove_prefix(kInclude.size());
    line = shipping_qt_detail::trim_ascii(line);
    if (line.empty()) {
        return false;
    }

    const char open = line.front();
    char close = '\0';
    if (open == '<') {
        close = '>';
    } else if (open == '"') {
        close = '"';
    } else {
        return false;
    }
    line.remove_prefix(1);
    const std::size_t end = line.find(close);
    if (end == std::string_view::npos) {
        return false;
    }

    const std::string_view name = shipping_qt_detail::trim_ascii(line.substr(0, end));
    std::size_t i = 0;
    while (i < name.size()) {
        while (i < name.size() && (name[i] == '/' || name[i] == '\\')) {
            ++i;
        }
        std::size_t j = i;
        while (j < name.size() && name[j] != '/' && name[j] != '\\') {
            ++j;
        }
        if (i < j && shipping_qt_detail::is_qt_segment(name.substr(i, j - i))) {
            return true;
        }
        i = j;
    }
    return false;
}

} // namespace fuse
