#pragma once

#include <fuse/compat/ts.hpp>

namespace fuse::compat::ts_t2d {

[[nodiscard]] inline constexpr Dialect dialect() {
    return Dialect::T2d;
}

[[nodiscard]] ExecResult eval(std::string_view source, std::string_view chunkName);
[[nodiscard]] ExecResult evalFile(const std::filesystem::path& path);

} // namespace fuse::compat::ts_t2d
