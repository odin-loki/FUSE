#pragma once

namespace fuse::hud {

/// HUD startup order. Earlier enumerators initialize first.
enum class HudModule { Platform, Renderer, World, Hud };

/// True when `first` is strictly earlier than `second` in HUD module init order.
[[nodiscard]] constexpr bool module_order_ok(HudModule first, HudModule second) {
    return static_cast<int>(first) < static_cast<int>(second);
}

} // namespace fuse::hud
