// Internal helpers shared by fuse_text's sources.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fuse::text::detail {

/// Reads a whole file; `utf8Path` is converted to the platform's path type (wide on Windows).
bool readFile(const std::string& utf8Path, std::vector<std::uint8_t>& out);

} // namespace fuse::text::detail
