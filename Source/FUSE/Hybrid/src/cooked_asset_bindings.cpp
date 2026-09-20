#include <fuse/hybrid/cooked_asset_bindings.hpp>

#include <fstream>

namespace fuse::hybrid {

namespace {

bool startsWith(const std::string& text, const char* prefix) {
    if (prefix == nullptr) {
        return false;
    }
    const std::string needle(prefix);
    return text.size() >= needle.size() && text.compare(0, needle.size(), needle) == 0;
}

} // namespace

void CookedAssetBindings::clear() {
    m_materials.clear();
    m_shaders.clear();
    m_materialTintR = 0.f;
    m_materialTintG = 0.f;
    m_materialTintB = 0.f;
    m_shaderTintR = 0.f;
    m_shaderTintG = 0.f;
    m_shaderTintB = 0.f;
}

bool CookedAssetBindings::probeCookedHeader(const std::string& path, const char* markerPrefix) {
    if (path.empty()) {
        return false;
    }

    std::ifstream in(path);
    if (!in) {
        return false;
    }

    std::string header;
    if (!std::getline(in, header)) {
        return false;
    }

    return startsWith(header, markerPrefix);
}

void CookedAssetBindings::bindMaterial(const std::string& cookedPath, u32 materialId) {
    if (cookedPath.empty()) {
        return;
    }

    CookedMaterialBinding binding{};
    binding.cookedPath = cookedPath;
    binding.materialId = materialId;
    binding.headerValid =
        probeCookedHeader(cookedPath, "FUSETEX_") || probeCookedHeader(cookedPath, "FUSEMESH_");
    m_materials.push_back(std::move(binding));
}

void CookedAssetBindings::bindShader(const std::string& cookedPath, u32 shaderId) {
    if (cookedPath.empty()) {
        return;
    }

    CookedShaderBinding binding{};
    binding.cookedPath = cookedPath;
    binding.shaderId = shaderId;
    binding.headerValid = probeCookedHeader(cookedPath, "FUSESHADER_");
    m_shaders.push_back(std::move(binding));
}

u32 CookedAssetBindings::validMaterialCount() const {
    u32 count = 0;
    for (const CookedMaterialBinding& binding : m_materials) {
        if (binding.headerValid) {
            ++count;
        }
    }
    return count;
}

u32 CookedAssetBindings::validShaderCount() const {
    u32 count = 0;
    for (const CookedShaderBinding& binding : m_shaders) {
        if (binding.headerValid) {
            ++count;
        }
    }
    return count;
}

void CookedAssetBindings::refreshTints() {
    const u32 materialValid = validMaterialCount();
    const u32 shaderValid = validShaderCount();

    m_materialTintR = materialValid > 0u ? 0.12f + 0.04f * static_cast<float>(materialValid) : 0.f;
    m_materialTintG = materialValid > 0u ? 0.08f : 0.f;
    m_materialTintB = 0.f;

    m_shaderTintR = 0.f;
    m_shaderTintG = shaderValid > 0u ? 0.10f + 0.03f * static_cast<float>(shaderValid) : 0.f;
    m_shaderTintB = shaderValid > 0u ? 0.14f : 0.f;
}

} // namespace fuse::hybrid
