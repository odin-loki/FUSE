// The SDF-quad backend's shaders (ui_sdf_quad.vert / .frag, fuse_ui_embed_shader). Its own translation unit: binaries
// that never call useSdfQuads do not link the SPIR-V.
#include "fuse/ui/render/ui_vk.hpp"

#if __has_include("kUiSdfQuadVert_spv.h") && __has_include("kUiSdfQuadFrag_spv.h")
#include "kUiSdfQuadFrag_spv.h"
#include "kUiSdfQuadVert_spv.h"
#define FUSE_UI_VK_HAS_SDF 1
#endif

namespace fuse::ui::render {

bool UiVkRenderer::sdfQuadsAvailable() {
#if defined(FUSE_UI_VK_HAS_SDF)
    return true;
#else
    return false;
#endif
}

bool useSdfQuads(UiVkDesc& desc) {
#if defined(FUSE_UI_VK_HAS_SDF)
    desc.backend = UiVkBackend::SdfQuads;
    desc.sdfVertex = kUiSdfQuadVert;
    desc.sdfFragment = kUiSdfQuadFrag;
    return true;
#else
    (void)desc;
    return false;
#endif
}

} // namespace fuse::ui::render
