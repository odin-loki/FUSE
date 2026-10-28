#pragma once

namespace fuse::script {

/// True only when a script component is both enabled and compiled.
[[nodiscard]] inline bool component_ticks(bool enabled, bool compiled) {
    return enabled && compiled;
}

} // namespace fuse::script
