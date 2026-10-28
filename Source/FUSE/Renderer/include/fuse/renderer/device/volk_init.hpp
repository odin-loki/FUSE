#pragma once

namespace fuse::renderer {

inline bool volk_init_required(bool loaderLinked, bool alreadyInitialized) {
    return loaderLinked && !alreadyInitialized;
}

} // namespace fuse::renderer
