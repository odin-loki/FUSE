#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::io {

/// Raw asset payload produced by the I/O lane (WP-04).
/// Workers fill this on job threads; the game thread owns live slots via HandleTable.
struct Asset {
    std::string virtualPath;
    std::vector<u8> bytes;
};

} // namespace fuse::io
