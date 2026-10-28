#pragma once

#include <algorithm>
#include <cstdint>

namespace fuse::renderer {

/// Physical-device class for the discrete-GPU preference score.
enum class DeviceClass {
    Other,
    Integrated,
    Discrete,
    Cpu,
};

/// Preference score for a physical device.
///
/// Base: Discrete 1000, Integrated 100, Cpu 10, Other 0.
/// Each 256 MiB of `vramBytes` adds 1, capped at 32.
/// A device that cannot present scores 0.
[[nodiscard]] inline constexpr int score_device(DeviceClass kind, std::uint64_t vramBytes, bool presentable) {
    if (!presentable) {
        return 0;
    }
    int base = 0;
    switch (kind) {
    case DeviceClass::Discrete:
        base = 1000;
        break;
    case DeviceClass::Integrated:
        base = 100;
        break;
    case DeviceClass::Cpu:
        base = 10;
        break;
    case DeviceClass::Other:
        base = 0;
        break;
    }
    const std::uint64_t vramBonus = std::min(vramBytes / (256ull * 1024 * 1024), 32ull);
    return base + static_cast<int>(vramBonus);
}

} // namespace fuse::renderer
