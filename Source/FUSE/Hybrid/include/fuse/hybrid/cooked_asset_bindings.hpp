#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::hybrid {

struct CookedMaterialBinding {
    std::string cookedPath;
    u32 materialId = 0;
    bool headerValid = false;
};

struct CookedShaderBinding {
    std::string cookedPath;
    u32 shaderId = 0;
    bool headerValid = false;
};

/// Headless-safe registry of cooked `.fusetex` / `.fuseshader` assets for hybrid render path.
class CookedAssetBindings {
public:
    void clear();

    void bindMaterial(const std::string& cookedPath, u32 materialId = 0);
    void bindShader(const std::string& cookedPath, u32 shaderId = 0);

    [[nodiscard]] u32 materialCount() const { return static_cast<u32>(m_materials.size()); }
    [[nodiscard]] u32 shaderCount() const { return static_cast<u32>(m_shaders.size()); }
    [[nodiscard]] u32 validMaterialCount() const;
    [[nodiscard]] u32 validShaderCount() const;

    [[nodiscard]] const std::vector<CookedMaterialBinding>& materials() const { return m_materials; }
    [[nodiscard]] const std::vector<CookedShaderBinding>& shaders() const { return m_shaders; }

    /// Tint factors derived from bound cooked asset headers (0..1).
    [[nodiscard]] float materialTintR() const { return m_materialTintR; }
    [[nodiscard]] float materialTintG() const { return m_materialTintG; }
    [[nodiscard]] float materialTintB() const { return m_materialTintB; }
    [[nodiscard]] float shaderTintR() const { return m_shaderTintR; }
    [[nodiscard]] float shaderTintG() const { return m_shaderTintG; }
    [[nodiscard]] float shaderTintB() const { return m_shaderTintB; }

    void refreshTints();

private:
    [[nodiscard]] static bool probeCookedHeader(const std::string& path, const char* markerPrefix);

    std::vector<CookedMaterialBinding> m_materials;
    std::vector<CookedShaderBinding> m_shaders;
    float m_materialTintR = 0.f;
    float m_materialTintG = 0.f;
    float m_materialTintB = 0.f;
    float m_shaderTintR = 0.f;
    float m_shaderTintG = 0.f;
    float m_shaderTintB = 0.f;
};

} // namespace fuse::hybrid
