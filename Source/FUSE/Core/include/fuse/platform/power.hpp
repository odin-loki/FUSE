#pragma once

namespace fuse::platform {

enum class PowerState {
    Normal,
    LowPower,
    Thermal,
    Background
};

/// Runtime power/lifecycle state (stub — returns Normal on desktop CI).
PowerState getPowerState();

} // namespace fuse::platform
