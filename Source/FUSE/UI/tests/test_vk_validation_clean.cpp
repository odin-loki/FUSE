// fuse_ui_vk_validation_clean (UI4 gate): the fuse_ui_vk_parity and fuse_ui_vk_damage scenarios (every kernel path:
// tiles, glyphs, CPU and external images, scissors, backdrop blur passes, incremental redraw) under
// VK_LAYER_KHRONOS_validation with synchronization validation; any validation warning or error fails the gate.
// Skips (77) without the layer or a Vulkan device.
#include "vk_headless.hpp"

#define main vkParityMain
#include "test_vk_parity.cpp"
#undef main
#define main vkDamageMain
#include "test_vk_damage.cpp"
#undef main

int main() {
    using fuse::ui::test::VkHeadless;
    if (!VkHeadless::validationAvailable()) {
        std::printf("SKIP: VK_LAYER_KHRONOS_validation not installed\n");
        return 77;
    }
    VkHeadless::validationRequested() = true;
    const int parity = vkParityMain();
    if (parity == 77) return 77;
    const int damage = vkDamageMain();
    const int messages = VkHeadless::validationMessages();
    std::printf("fuse_ui_vk_validation_clean: parity %d, damage %d, %d validation message(s)\n", parity, damage, messages);
    if (parity != 0 || damage != 0 || messages != 0) return 1;
    std::printf("fuse_ui_vk_validation_clean: OK\n");
    return 0;
}
