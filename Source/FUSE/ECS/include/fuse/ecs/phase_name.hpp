#pragma once

namespace fuse::ecs {

/// Schedule phase name: 0 Input, 1 Simulate, 2 Present. Any other value is Unknown.
[[nodiscard]] inline constexpr const char* phase_name(int phase) {
    switch (phase) {
    case 0:
        return "Input";
    case 1:
        return "Simulate";
    case 2:
        return "Present";
    default:
        return "Unknown";
    }
}

} // namespace fuse::ecs
