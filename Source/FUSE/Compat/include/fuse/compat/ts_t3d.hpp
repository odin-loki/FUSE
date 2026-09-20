#pragma once

#include <fuse/compat/ts.hpp>

namespace fuse::compat::ts_t3d {

[[nodiscard]] inline constexpr Dialect dialect() {
    return Dialect::T3d;
}

[[nodiscard]] ExecResult eval(std::string_view source, std::string_view chunkName);
[[nodiscard]] ExecResult evalFile(const std::filesystem::path& path);

} // namespace fuse::compat::ts_t3d
