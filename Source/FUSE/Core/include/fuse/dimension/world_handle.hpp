#pragma once

#include <fuse/handle.hpp>

namespace fuse::dimension {

/// Opaque world identity — dimensions load content by handle, not raw pointers.
struct World {};

using WorldHandle = Handle<World>;

} // namespace fuse::dimension
