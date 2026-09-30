#include <fuse/hybrid/cooked_asset_bindings.hpp>

#include <fuse/asset/cooked_material.hpp>
#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/asset/cooked_texture.hpp>

#if defined(FUSE_HYBRID_HAS_COOKED_REGISTRY)
#include <fuse/renderer/cooked_assets/cooked_asset_registry.hpp>
#include <fuse/renderer/material_layers/ml_types.hpp>
#endif

#include <fstream>
#include <iterator>

namespace fuse::hybrid {

namespace {

std::vector<u8> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// E06: what the runtime readers recognise (Unknown when none does).
CookedHeaderKind parsedKind(const std::vector<u8>& bytes) {
    if (bytes.empty()) {
        return CookedHeaderKind::Unknown;
    }
    asset::CookedTexture texture;
    if (asset::parse_cooked_texture(bytes.data(), bytes.size(), texture)) {
        return texture.format == asset::BcFormat::BC7 ? CookedHeaderKind::TextureBc7 : CookedHeaderKind::TextureBcn;
    }
    asset::CookedMaterial material;
    if (asset::read_cooked_material(bytes.data(), bytes.size(), material)) {
        return CookedHeaderKind::MaterialFusemat;
    }
    asset::CookedMesh mesh;
    if (asset::deserialize_cooked_mesh(bytes.data(), bytes.size(), mesh)) {
        return CookedHeaderKind::MeshFmsh;
    }
    return CookedHeaderKind::Unknown;
}

bool startsWith(const std::string& text, const char* prefix) {
    if (prefix == nullptr) {
        return false;
    }
    const std::string needle(prefix);
    return text.size() >= needle.size() && text.compare(0, needle.size(), needle) == 0;
}

CookedHeaderKind headerKindFromMarker(const std::string& marker) {
    if (marker == "FUSETEX_BC7") {
        return CookedHeaderKind::TextureBc7;
    }
    if (startsWith(marker, "FUSETEX_")) {
        return CookedHeaderKind::TextureStub;
    }
    if (startsWith(marker, "FUSEMESH_")) {
        return CookedHeaderKind::MeshStub;
    }
    if (marker == "FUSESHADER_GLSLANG") {
        return CookedHeaderKind::ShaderGlslang;
    }
    if (marker == "FUSESHADER_SPIV") {
        return CookedHeaderKind::ShaderSpiv;
    }
    if (startsWith(marker, "FUSESHADER_")) {
        return CookedHeaderKind::ShaderStub;
    }
    return CookedHeaderKind::Unknown;
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

CookedHeaderKind CookedAssetBindings::probeCookedHeaderKind(const std::string& path) {
    if (path.empty()) {
        return CookedHeaderKind::Unknown;
    }
    // E06: real cooked files through the runtime readers first; the marker line only identifies placeholder stubs.
    const CookedHeaderKind parsed = parsedKind(readFile(path));
    if (parsed != CookedHeaderKind::Unknown) {
        return parsed;
    }

    std::ifstream in(path);
    if (!in) {
        return CookedHeaderKind::Unknown;
    }

    std::string header;
    if (!std::getline(in, header)) {
        return CookedHeaderKind::Unknown;
    }

    return headerKindFromMarker(header);
}

void CookedAssetBindings::bindMaterial(const std::string& cookedPath, u32 materialId) {
    if (cookedPath.empty()) {
        return;
    }

    CookedMaterialBinding binding{};
    binding.cookedPath = cookedPath;
    binding.materialId = materialId;
    binding.headerKind = probeCookedHeaderKind(cookedPath);
    binding.headerValid = binding.headerKind != CookedHeaderKind::Unknown;
    uploadBinding(binding);
    m_materials.push_back(std::move(binding));
}

void CookedAssetBindings::attachRegistry(renderer::cooked_assets::CookedAssetRegistry* registry) {
    m_registry = registry;
    m_uploadedRows.clear();
    for (CookedMaterialBinding& binding : m_materials) {
        binding.uploaded = false;
        binding.textureIndex = 0xFFFFFFFFu;
        binding.bindlessHandle = 0;
        binding.gpuMaterialRow = 0xFFFFFFFFu;
        uploadBinding(binding);
    }
}

u32 CookedAssetBindings::uploadedMaterialCount() const {
    u32 count = 0;
    for (const CookedMaterialBinding& binding : m_materials) {
        count += binding.uploaded ? 1u : 0u;
    }
    return count;
}

void CookedAssetBindings::uploadBinding(CookedMaterialBinding& binding) {
#if defined(FUSE_HYBRID_HAS_COOKED_REGISTRY)
    if (m_registry == nullptr || !m_registry->valid()) {
        return;
    }
    namespace ca = renderer::cooked_assets;
    if (binding.headerKind == CookedHeaderKind::TextureBc7 || binding.headerKind == CookedHeaderKind::TextureBcn) {
        u32 index = m_registry->findTexture(binding.cookedPath);
        if (index == renderer::material_layers::kMlNoTexture) {
            const std::vector<u8> bytes = readFile(binding.cookedPath);
            asset::CookedTexture texture;
            if (asset::parse_cooked_texture(bytes.data(), bytes.size(), texture)) {
                index = m_registry->addCookedTexture(binding.cookedPath, texture);
            }
        }
        if (index != renderer::material_layers::kMlNoTexture) {
            binding.uploaded = true;
            binding.textureIndex = index;
            binding.bindlessHandle = m_registry->textureHandle(index);
        }
    } else if (binding.headerKind == CookedHeaderKind::MaterialFusemat) {
        for (const auto& [path, row] : m_uploadedRows) {
            if (path == binding.cookedPath) {
                binding.uploaded = true;
                binding.gpuMaterialRow = row;
                return;
            }
        }
        const std::vector<u8> bytes = readFile(binding.cookedPath);
        asset::CookedMaterial material;
        if (asset::read_cooked_material(bytes.data(), bytes.size(), material)) {
            const u32 row = m_registry->addMaterial(ca::fusemat_from_cooked(material));
            if (row != ca::CookedAssetRegistry::kInvalid) {
                binding.uploaded = true;
                binding.gpuMaterialRow = row;
                m_uploadedRows.emplace_back(binding.cookedPath, row);
            }
        }
    }
#else
    (void)binding;
#endif
}

void CookedAssetBindings::bindShader(const std::string& cookedPath, u32 shaderId) {
    if (cookedPath.empty()) {
        return;
    }

    CookedShaderBinding binding{};
    binding.cookedPath = cookedPath;
    binding.shaderId = shaderId;
    binding.headerKind = probeCookedHeaderKind(cookedPath);
    binding.headerValid = binding.headerKind != CookedHeaderKind::Unknown;
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

u32 CookedAssetBindings::bc7MaterialCount() const {
    u32 count = 0;
    for (const CookedMaterialBinding& binding : m_materials) {
        if (binding.headerKind == CookedHeaderKind::TextureBc7) {
            ++count;
        }
    }
    return count;
}

u32 CookedAssetBindings::glslangShaderCount() const {
    u32 count = 0;
    for (const CookedShaderBinding& binding : m_shaders) {
        if (binding.headerKind == CookedHeaderKind::ShaderGlslang) {
            ++count;
        }
    }
    return count;
}

std::optional<CookedMaterialBinding> CookedAssetBindings::findMaterial(u32 materialId) const {
    for (const CookedMaterialBinding& binding : m_materials) {
        if (binding.materialId == materialId && binding.headerValid) {
            return binding;
        }
    }
    return std::nullopt;
}

std::optional<CookedShaderBinding> CookedAssetBindings::findShader(u32 shaderId) const {
    for (const CookedShaderBinding& binding : m_shaders) {
        if (binding.shaderId == shaderId && binding.headerValid) {
            return binding;
        }
    }
    return std::nullopt;
}

float CookedAssetBindings::materialTintBoost(u32 materialId) const {
    const std::optional<CookedMaterialBinding> binding = findMaterial(materialId);
    if (!binding.has_value()) {
        return 0.f;
    }

    switch (binding->headerKind) {
    case CookedHeaderKind::TextureBc7:
    case CookedHeaderKind::TextureBcn:
        return 0.08f;
    case CookedHeaderKind::TextureStub:
        return 0.04f;
    case CookedHeaderKind::MeshStub:
        return 0.03f;
    default:
        return 0.02f;
    }
}

void CookedAssetBindings::refreshTints() {
    const u32 materialValid = validMaterialCount();
    const u32 shaderValid = validShaderCount();
    const u32 bc7Count = bc7MaterialCount();
    const u32 glslangCount = glslangShaderCount();

    m_materialTintR = materialValid > 0u ? 0.10f + 0.03f * static_cast<float>(materialValid) : 0.f;
    m_materialTintG = materialValid > 0u ? 0.06f + 0.02f * static_cast<float>(bc7Count) : 0.f;
    m_materialTintB = bc7Count > 0u ? 0.04f : 0.f;

    m_shaderTintR = 0.f;
    m_shaderTintG = shaderValid > 0u ? 0.08f + 0.02f * static_cast<float>(shaderValid) : 0.f;
    m_shaderTintB = shaderValid > 0u ? 0.12f + 0.03f * static_cast<float>(glslangCount) : 0.f;
}

} // namespace fuse::hybrid
