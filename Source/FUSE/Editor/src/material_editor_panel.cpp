#include <fuse/editor/material_editor_panel.hpp>

#include <fuse/editor/material_property_inspect.hpp>
#include <fuse/handle.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/object.hpp>

#if defined(FUSE_EDITOR_HAS_RHI)
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <string>

namespace fuse::editor {

namespace {

Handle<Object> materialHandle(u32 materialId) {
    return Handle<Object>(materialId, 1u);
}

/// NaN and -Inf land on 0, +Inf on 1, everything else on [0, 1].
f32 clampPreview01(f32 value) {
    if (!std::isfinite(value)) {
        return value > 0.f ? 1.f : 0.f;
    }
    return std::clamp(value, 0.f, 1.f);
}

u8 encodePreviewByte(f32 value) {
    if (!(value > 0.f)) {
        return 0u;
    }
    if (value >= 1.f) {
        return 255u;
    }
    return static_cast<u8>(value * 255.f + 0.5f);
}

} // namespace

void MaterialEditorPanel::bindSelectedMaterial_() {
    if (!m_binding.tryBind(m_selectedMatId, m_catalogCount, m_editState)) {
        m_selectedMatId = kInvalidMaterialId;
    }
}

void MaterialEditorPanel::unbindSelectedMaterial_() {
    m_binding.unbind();
}

void MaterialEditorPanel::markEditDirty_() {
    m_previewDirty = true;
    m_editDirty = true;
}

void MaterialEditorPanel::sync(const EditorState& /*state*/, u32 materialCount) {
    m_catalogCount = materialCount;
    if (m_selectedMatId >= m_catalogCount) {
        m_selectedMatId = kInvalidMaterialId;
        unbindSelectedMaterial_();
    }
    m_previewDirty = true;
}

void MaterialEditorPanel::syncFromMaterialSystem(const EditorState& state,
                                                 renderer::MaterialSystem& materials) {
#if defined(FUSE_EDITOR_HAS_RHI)
    sync(state, materials.materialCount());
    if (m_selectedMatId == kInvalidMaterialId && m_catalogCount > 0u) {
        selectMaterial(0u);
    }

    if (m_selectedMatId != kInvalidMaterialId && m_selectedMatId < materials.materialCount()) {
        const renderer::Material& material = materials.get(m_selectedMatId);
        m_editState.baseColorR = material.baseColor.x;
        m_editState.baseColorG = material.baseColor.y;
        m_editState.baseColorB = material.baseColor.z;
        m_editState.roughness = material.roughness;
        m_editState.metallic = material.metallic;
        m_editState.shadingModel = static_cast<u8>(material.shadingModel);
        m_editState.procedural = material.isProcedural;
        m_editState.proceduralFnId = material.proceduralFnId;
        m_editState.proceduralSeed = material.proceduralSeed;
        m_binding.refreshFromEditState(m_editState);
        m_previewDirty = false;
        m_editDirty = false;
    }
#else
    (void)materials;
    sync(state, 0u);
#endif
}

bool MaterialEditorPanel::selectMaterial(u32 materialId) {
    if (!canSelectMaterial(materialId)) {
        return false;
    }

    m_selectedMatId = materialId;
    bindSelectedMaterial_();
    if (!hasSelectedMaterial()) {
        return false;
    }
    m_previewDirty = true;
    return true;
}

bool MaterialEditorPanel::shouldSkipPropertyEdit() const {
    return isCatalogEmpty() || !hasSelectedMaterial() || !m_binding.canPostProperty();
}

bool MaterialEditorPanel::shouldRefreshPanel() const {
    return previewDirty() || needsPanelRefresh();
}

bool MaterialEditorPanel::canApplyPanelRefresh() const {
    return hasSelectedMaterial() && m_binding.isBound();
}

bool MaterialEditorPanel::canRefreshPanel() const {
    return hasSelectedMaterial() && needsPanelRefresh();
}

void MaterialEditorPanel::refreshPanel() {
    m_binding.markPanelRefreshed();
    m_previewDirty = false;
}

bool MaterialEditorPanel::tryRefreshPanel() {
    if (!canRefreshPanel()) {
        return false;
    }
    refreshPanel();
    return true;
}

bool MaterialEditorPanel::renderPreview(u32 width, u32 height, u8* rgba8) const {
    if (width == 0u || height == 0u || rgba8 == nullptr) {
        return false;
    }

    const f32 roughness = clampPreview01(m_editState.roughness);
    const f32 metallic = clampPreview01(m_editState.metallic);
    const f32 baseR = clampPreview01(m_editState.baseColorR);
    const f32 baseG = clampPreview01(m_editState.baseColorG);
    const f32 baseB = clampPreview01(m_editState.baseColorB);

    // Roughness 0 -> exponent 256 (tight highlight). Roughness 1 -> exponent 1 (wide).
    const f32 specularExponent = std::exp2((1.f - roughness) * 8.f);
    const f32 diffuseScale = 1.f - metallic;
    constexpr f32 kDielectricF0 = 0.04f;
    const f32 specR = kDielectricF0 + (baseR - kDielectricF0) * metallic;
    const f32 specG = kDielectricF0 + (baseG - kDielectricF0) * metallic;
    const f32 specB = kDielectricF0 + (baseB - kDielectricF0) * metallic;

    const math::Vec3 light = math::Vec3{0.3f, 0.8f, 0.4f}.normalized();
    const math::Vec3 view{0.f, 0.f, 1.f};
    const math::Vec3 half = (light + view).normalized();

    const f32 minAxis = static_cast<f32>(width < height ? width : height);
    const f32 xExtent = static_cast<f32>(width) / minAxis;
    const f32 yExtent = static_cast<f32>(height) / minAxis;
    const f32 invWidth = 1.f / static_cast<f32>(width);
    const f32 invHeight = 1.f / static_cast<f32>(height);

    for (u32 y = 0u; y < height; ++y) {
        const f32 py = (1.f - (static_cast<f32>(y) + 0.5f) * invHeight * 2.f) * yExtent;
        for (u32 x = 0u; x < width; ++x) {
            const f32 px = ((static_cast<f32>(x) + 0.5f) * invWidth * 2.f - 1.f) * xExtent;
            u8* pixel = rgba8 + (static_cast<usize>(y) * width + x) * 4u;
            const f32 r2 = px * px + py * py;
            if (r2 > 1.f) {
                pixel[0] = 0u;
                pixel[1] = 0u;
                pixel[2] = 0u;
                pixel[3] = 0u;
                continue;
            }

            const math::Vec3 normal{px, py, std::sqrt(1.f - r2)};
            const f32 nDotL = std::max(normal.dot(light), 0.f);
            const f32 nDotH = std::max(normal.dot(half), 0.f);
            const f32 lambert = nDotL * diffuseScale;
            const f32 specular = std::pow(nDotH, specularExponent) * nDotL;
            pixel[0] = encodePreviewByte(baseR * lambert + specR * specular);
            pixel[1] = encodePreviewByte(baseG * lambert + specG * specular);
            pixel[2] = encodePreviewByte(baseB * lambert + specB * specular);
            pixel[3] = 255u;
        }
    }
    return true;
}

bool MaterialEditorPanel::setRoughness(f32 roughness, CommandStack& cmds) {
    if (shouldSkipPropertyEdit()) {
        return false;
    }

    markEditDirty_();
    return m_binding.setRoughness(roughness, cmds);
}

bool MaterialEditorPanel::setMetallic(f32 metallic, CommandStack& cmds) {
    if (shouldSkipPropertyEdit()) {
        return false;
    }

    markEditDirty_();
    return m_binding.setMetallic(metallic, cmds);
}

bool MaterialEditorPanel::setBaseColor(f32 r, f32 g, f32 b, CommandStack& cmds) {
    if (shouldSkipPropertyEdit()) {
        return false;
    }

    markEditDirty_();
    return m_binding.setBaseColor(r, g, b, cmds);
}

bool MaterialEditorPanel::setShadingModel(u8 shadingModel, CommandStack& cmds) {
    if (shouldSkipPropertyEdit()) {
        return false;
    }

    markEditDirty_();
    return m_binding.setShadingModel(shadingModel, cmds);
}

bool MaterialEditorPanel::setBaseColorSrgb(f32 r, f32 g, f32 b, CommandStack& cmds) {
    return setBaseColor(srgbToLinear(r), srgbToLinear(g), srgbToLinear(b), cmds);
}

void MaterialEditorPanel::baseColorSrgb(f32& r, f32& g, f32& b) const {
    r = linearToSrgb(m_editState.baseColorR);
    g = linearToSrgb(m_editState.baseColorG);
    b = linearToSrgb(m_editState.baseColorB);
}

bool MaterialEditorPanel::setProceduralMaterial(u32 fnId, u32 seed, CommandStack& cmds) {
    if (shouldSkipPropertyEdit()) {
        return false;
    }

    const std::string before = m_editState.procedural ? std::to_string(m_editState.proceduralFnId) : "0";
    m_editState.procedural = fnId != 0u;
    m_editState.proceduralFnId = fnId;
    m_editState.proceduralSeed = seed;
    markEditDirty_();

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = materialHandle(m_selectedMatId);
    command.propertyName = "material.procedural";
    command.propertyValue = std::to_string(fnId);
    cmds.push(std::move(command), before);
    return true;
}

bool MaterialEditorPanel::pushToMaterialSystem(renderer::MaterialSystem& materials,
                                              CommandStack& cmds) {
#if defined(FUSE_EDITOR_HAS_RHI)
    if (m_selectedMatId == kInvalidMaterialId || !materials.isReady()) {
        return false;
    }

    if (m_selectedMatId >= materials.materialCount()) {
        return false;
    }

    renderer::Material material = materials.get(m_selectedMatId);
    material.baseColor.x = m_editState.baseColorR;
    material.baseColor.y = m_editState.baseColorG;
    material.baseColor.z = m_editState.baseColorB;
    material.roughness = m_editState.roughness;
    material.metallic = m_editState.metallic;
    material.shadingModel = static_cast<renderer::ShadingModel>(m_editState.shadingModel);
    material.isProcedural = m_editState.procedural;
    material.proceduralFnId = m_editState.procedural ? m_editState.proceduralFnId : 0u;
    material.proceduralSeed = m_editState.proceduralSeed;
    materials.updateMaterial(m_selectedMatId, material);

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = materialHandle(m_selectedMatId);
    command.propertyName = "material";
    command.propertyValue = std::to_string(m_selectedMatId);
    cmds.execute(std::move(command));
    refreshPanel();
    m_editDirty = false;
    return true;
#else
    (void)materials;
    (void)cmds;
    return false;
#endif
}

} // namespace fuse::editor
