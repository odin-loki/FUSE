// UiVkRenderer::setEffects and the built-in effect SPIR-V (fuse_ui_add_effect, Render/cmake/fuse_ui_effects.cmake).
// Its own translation unit: binaries that never call setEffects do not link the SPIR-V.
#include "fuse/ui/effects.hpp"
#include "fuse/ui/render/ui_vk.hpp"

#if __has_include("kUiFxDesaturate_spv.h")
#include "kUiFxDesaturate_spv.h"
#include "kUiFxGrain_spv.h"
#include "kUiFxRipple_spv.h"
#define FUSE_UI_VK_HAS_FX 1
#endif

namespace fuse::ui::render {

std::span<const u32> UiVkRenderer::builtinEffectSpirv(std::string_view name) {
#if defined(FUSE_UI_VK_HAS_FX)
    if (name == "desaturate") return kUiFxDesaturate;
    if (name == "ripple") return kUiFxRipple;
    if (name == "grain") return kUiFxGrain;
#endif
    (void)name;
    return {};
}

bool UiVkRenderer::setEffects(const EffectRegistry& registry) {
    bool ok = valid();
    for (u32 id = 0; id < kMaxEffects && ok; ++id) {
        std::span<const u32> spirv;
        if (id < registry.count()) {
            const EffectDesc& d = registry.at(id);
            spirv = d.spirv.empty() ? builtinEffectSpirv(d.name) : d.spirv;
        }
        ok = setEffect(id, spirv);
    }
    return ok;
}

} // namespace fuse::ui::render
