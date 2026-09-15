#include <fuse/project/json_reader.hpp>

#include <cctype>

namespace fuse::project::json {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

Reader::Reader(std::string_view source) : m_source(source) {}

std::string_view Reader::findObject(std::string_view objectKey) const {
    const std::string needle = std::string("\"") + std::string(objectKey) + "\":";
    const std::size_t keyPos = m_source.find(needle);
    if (keyPos == std::string_view::npos) {
        return {};
    }

    std::size_t cursor = keyPos + needle.size();
    while (cursor < m_source.size() && std::isspace(static_cast<unsigned char>(m_source[cursor]))) {
        ++cursor;
    }
    if (cursor >= m_source.size() || m_source[cursor] != '{') {
        return {};
    }

    std::size_t depth = 0;
    for (std::size_t i = cursor; i < m_source.size(); ++i) {
        const char ch = m_source[i];
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return m_source.substr(cursor, i - cursor + 1);
            }
        }
    }
    return {};
}

std::string_view Reader::findFieldValue(std::string_view objectBody, std::string_view fieldKey) const {
    const std::string needle = std::string("\"") + std::string(fieldKey) + "\":";
    const std::size_t keyPos = objectBody.find(needle);
    if (keyPos == std::string_view::npos) {
        return {};
    }

    std::size_t cursor = keyPos + needle.size();
    while (cursor < objectBody.size() && std::isspace(static_cast<unsigned char>(objectBody[cursor]))) {
        ++cursor;
    }

    std::size_t end = cursor;
    if (end < objectBody.size() && objectBody[end] == '"') {
        ++end;
        while (end < objectBody.size() && objectBody[end] != '"') {
            if (objectBody[end] == '\\' && end + 1 < objectBody.size()) {
                end += 2;
            } else {
                ++end;
            }
        }
        if (end < objectBody.size()) {
            ++end;
        }
        return objectBody.substr(cursor, end - cursor);
    }

    while (end < objectBody.size()) {
        const char ch = objectBody[end];
        if (ch == ',' || ch == '}') {
            break;
        }
        ++end;
    }
    return trim(objectBody.substr(cursor, end - cursor));
}

bool Reader::parseBool(std::string_view token, bool& out) const {
    const std::string_view value = trim(token);
    if (value == "true") {
        out = true;
        return true;
    }
    if (value == "false") {
        out = false;
        return true;
    }
    return false;
}

bool Reader::parseU32(std::string_view token, u32& out) const {
    const std::string_view value = trim(token);
    if (value.empty()) {
        return false;
    }

    u32 parsed = 0;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        parsed = parsed * 10u + static_cast<u32>(ch - '0');
    }
    out = parsed;
    return true;
}

std::string Reader::parseString(std::string_view token) const {
    std::string_view value = trim(token);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return std::string(value);
}

bool Reader::readU32(std::string_view key, u32& out) const {
    const std::string_view value = findFieldValue(m_source, key);
    if (value.empty()) {
        m_error = "missing field: ";
        m_error.append(key);
        return false;
    }
    if (!parseU32(value, out)) {
        m_error = "invalid integer for field: ";
        m_error.append(key);
        return false;
    }
    return true;
}

bool Reader::readString(std::string_view key, std::string& out) const {
    const std::string_view value = findFieldValue(m_source, key);
    if (value.empty()) {
        return false;
    }
    out = parseString(value);
    return true;
}

bool Reader::readBool(std::string_view objectKey, std::string_view fieldKey, bool& out) const {
    const std::string_view objectBody = findObject(objectKey);
    if (objectBody.empty()) {
        return false;
    }

    const std::string_view value = findFieldValue(objectBody, fieldKey);
    if (value.empty()) {
        return false;
    }

    return parseBool(value, out);
}

} // namespace fuse::project::json
