#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace fuse::compat {

enum class Dialect {
    T3d,
    T2d,
};

struct ExecResult {
    bool ok = false;
    std::string output;
    std::string error;
    Dialect dialect = Dialect::T3d;
};

[[nodiscard]] ExecResult eval(Dialect dialect, std::string_view source, std::string_view chunkName);
[[nodiscard]] ExecResult evalFile(Dialect dialect, const std::filesystem::path& path);

} // namespace fuse::compat
