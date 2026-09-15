#pragma once

#include <functional>

namespace fuse::platform {

enum class PowerState {
    Normal,
    LowPower,
    Thermal,
    Background
};

using PowerStateCallback = std::function<void(PowerState previous, PowerState current)>;

/// Runtime power/lifecycle state (updated by lifecycle hooks and platform backends).
PowerState getPowerState();

void registerPowerStateCallback(PowerStateCallback callback);

void clearPowerStateCallbacks();

/// Platform backends / tests set explicit power state (e.g. thermal throttle).
void setPowerState(PowerState state);

} // namespace fuse::platform
