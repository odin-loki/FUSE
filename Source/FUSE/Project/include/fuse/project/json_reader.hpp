#pragma once

#include <fuse/types.hpp>

#include <string>
#include <string_view>

namespace fuse::project::json {

/// Minimal JSON reader for the fixed `project.json` schema (no external deps).
class Reader {
public:
    explicit Reader(std::string_view source);

    bool hasError() const { return !m_error.empty(); }
    const std::string& error() const { return m_error; }

    bool readU32(std::string_view key, u32& out) const;
    bool readString(std::string_view key, std::string& out) const;
    bool readBool(std::string_view objectKey, std::string_view fieldKey, bool& out) const;

private:
    std::string_view m_source;
    mutable std::string m_error;

    std::string_view findObject(std::string_view objectKey) const;
    std::string_view findFieldValue(std::string_view objectBody, std::string_view fieldKey) const;
    bool parseBool(std::string_view token, bool& out) const;
    bool parseU32(std::string_view token, u32& out) const;
    std::string parseString(std::string_view token) const;
};

} // namespace fuse::project::json
