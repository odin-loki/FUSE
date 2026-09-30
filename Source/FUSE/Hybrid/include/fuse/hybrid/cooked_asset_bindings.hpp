#pragma once

#include <fuse/types.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fuse::renderer::cooked_assets {
class CookedAssetRegistry;
}

namespace fuse::hybrid {

enum class CookedHeaderKind : u8 {
    Unknown = 0,
    TextureStub = 1,
    TextureBc7 = 2,
    MeshStub = 3,
    ShaderStub = 4,
    ShaderSpiv = 5,
    ShaderGlslang = 6,
    // E06: real cooked content, recognised by the runtime readers (fuse_asset), not by a header marker.
    TextureBcn = 7,      ///< `.fusetex` BC1 / BC4 / BC5 / BC6H
    MaterialFusemat = 8, ///< `.fusemat`
    MeshFmsh = 9,        ///< `.fusemesh` (FMSH v1 / v2)
};

struct CookedMaterialBinding {
    std::string cookedPath;
    u32 materialId = 0;
    bool headerValid = false;
    CookedHeaderKind headerKind = CookedHeaderKind::Unknown;
    // E06: set when a renderer registry is attached (attachRegistry) and the asset was uploaded into it.
    bool uploaded = false;
    u32 textureIndex = 0xFFFFFFFFu;   ///< layered-table texture index (textures)
    u32 bindlessHandle = 0;           ///< bindless sampled-image handle (textures; 0 without a device)
    u32 gpuMaterialRow = 0xFFFFFFFFu; ///< GpuScene material row (.fusemat)
};

struct CookedShaderBinding {
    std::string cookedPath;
    u32 shaderId = 0;
    bool headerValid = false;
    CookedHeaderKind headerKind = CookedHeaderKind::Unknown;
};

/// Headless-safe registry of cooked `.fusetex` / `.fuseshader` assets for hybrid render path.
///
/// E06 (AP-RT-COOKED): bound files are read with the runtime readers (fuse_asset: .fusetex / .fusemat / .fusemesh);
/// the first-line marker probe remains only for labelled placeholder stubs. With a renderer registry attached
/// (attachRegistry: renderer::cooked_assets::CookedAssetRegistry, the SceneRenderer's cooked-asset sink) textures are
/// uploaded (bindless slot, cook id = the path / its stem) and .fusemat materials become GpuScene rows, reported back
/// in the binding. The tints below are the software placeholder renderer's cue only.
class CookedAssetBindings {
public:
    void clear();

    /// E06: uploads bound (and later bound) cooked textures / materials into `registry` (null detaches). The registry
    /// keeps what was uploaded across clear(); rebinding the same path reuses it.
    void attachRegistry(renderer::cooked_assets::CookedAssetRegistry* registry);
    [[nodiscard]] renderer::cooked_assets::CookedAssetRegistry* registry() const { return m_registry; }
    [[nodiscard]] u32 uploadedMaterialCount() const;

    void bindMaterial(const std::string& cookedPath, u32 materialId = 0);
    void bindShader(const std::string& cookedPath, u32 shaderId = 0);

    [[nodiscard]] u32 materialCount() const { return static_cast<u32>(m_materials.size()); }
    [[nodiscard]] u32 shaderCount() const { return static_cast<u32>(m_shaders.size()); }
    [[nodiscard]] u32 validMaterialCount() const;
    [[nodiscard]] u32 validShaderCount() const;
    [[nodiscard]] u32 bc7MaterialCount() const;
    [[nodiscard]] u32 glslangShaderCount() const;

    [[nodiscard]] const std::vector<CookedMaterialBinding>& materials() const { return m_materials; }
    [[nodiscard]] const std::vector<CookedShaderBinding>& shaders() const { return m_shaders; }

    [[nodiscard]] std::optional<CookedMaterialBinding> findMaterial(u32 materialId) const;
    [[nodiscard]] std::optional<CookedShaderBinding> findShader(u32 shaderId) const;

    /// Tint factors derived from bound cooked asset headers (0..1).
    [[nodiscard]] float materialTintR() const { return m_materialTintR; }
    [[nodiscard]] float materialTintG() const { return m_materialTintG; }
    [[nodiscard]] float materialTintB() const { return m_materialTintB; }
    [[nodiscard]] float shaderTintR() const { return m_shaderTintR; }
    [[nodiscard]] float shaderTintG() const { return m_shaderTintG; }
    [[nodiscard]] float shaderTintB() const { return m_shaderTintB; }

    /// Per-material tint boost when a valid cooked texture is bound for `materialId`.
    [[nodiscard]] float materialTintBoost(u32 materialId) const;

    void refreshTints();

private:
    [[nodiscard]] static CookedHeaderKind probeCookedHeaderKind(const std::string& path);
    void uploadBinding(CookedMaterialBinding& binding);

    renderer::cooked_assets::CookedAssetRegistry* m_registry = nullptr;
    std::vector<std::pair<std::string, u32>> m_uploadedRows; ///< .fusemat path -> GpuScene row (kept across clear)

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
