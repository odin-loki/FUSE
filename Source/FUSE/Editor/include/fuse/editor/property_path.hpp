#pragma once

#include <string>
#include <string_view>

namespace fuse::editor {

inline std::string join_property_path(std::string_view objectName, std::string_view property) {
    auto trim = [](std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
            text.remove_prefix(1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
            text.remove_suffix(1);
        }
        return text;
    };
    const std::string_view object = trim(objectName);
    const std::string_view field = trim(property);
    std::string out;
    if (object.empty()) {
        out.assign(field);
    } else if (field.empty()) {
        out.assign(object);
    } else {
        out.reserve(object.size() + field.size() + 1);
        out.assign(object);
        out.push_back('.');
        out.append(field);
    }
    if (out.size() > 128) {
        out.resize(128);
    }
    return out;
}

} // namespace fuse::editor
