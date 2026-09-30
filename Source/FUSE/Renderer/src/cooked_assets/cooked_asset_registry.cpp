// E06 (AP-RT-COOKED): see include/fuse/renderer/cooked_assets/cooked_asset_registry.hpp.
#include <fuse/renderer/cooked_assets/cooked_asset_registry.hpp>

#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/renderer/cooked_assets/bcn_decode.hpp>
#include <fuse/renderer/frame/frame_composer.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material_layers/ml_mips.hpp>
#include <fuse/renderer/material_layers/ml_reference.hpp>
#include <fuse/renderer/scene_renderer/mesh_registry.hpp>
#include <fuse/renderer/scene_renderer/scene_renderer.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::renderer::cooked_assets {

namespace ml = material_layers;

namespace {

u64 align256(u64 v) { return (v + 255u) & ~u64{255u}; }

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

/// Cook ids of a virtual path: the path, the path without its extension, the bare stem.
std::vector<std::string> cookIdsOf(std::string_view path) {
    std::vector<std::string> ids;
    if (path.empty()) {
        return ids;
    }
    ids.emplace_back(path);
    const usize slash = path.find_last_of("/\\:");
    const usize dot = path.find_last_of('.');
    const bool hasExt = dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash);
    if (hasExt) {
        ids.emplace_back(path.substr(0, dot));
    }
    const usize begin = slash == std::string_view::npos ? 0u : slash + 1u;
    const usize end = hasExt ? dot : path.size();
    if (end > begin) {
        std::string stem(path.substr(begin, end - begin));
        if (std::find(ids.begin(), ids.end(), stem) == ids.end()) {
            ids.push_back(std::move(stem));
        }
    }
    return ids;
}

/// Decoded mean of RGBA8 words (sRGB RGB through the LUT), the histogram-preserving blend's mean.
void textureMean(const std::vector<u32>& texels, bool srgb, f32 out[4]) {
    static const struct Lut {
        f32 v[ml::kMlLutEntries];
        Lut() { ml::ml_build_lut(v); }
    } lut;
    f64 sum[4] = {};
    for (const u32 w : texels) {
        for (u32 c = 0; c < 4u; ++c) {
            const u32 b = (w >> (8u * c)) & 255u;
            sum[c] += static_cast<f64>(c < 3u && srgb ? lut.v[b] : lut.v[256u + b]);
        }
    }
    const f64 n = texels.empty() ? 1.0 : static_cast<f64>(texels.size());
    for (u32 c = 0; c < 4u; ++c) {
        out[c] = static_cast<f32>(sum[c] / n);
    }
}

} // namespace

// ================================================================================================================
ml::FuseMat fusemat_from_cooked(const asset::CookedMaterial& c) {
    ml::FuseMat m{};
    m.version = c.version;
    m.name = c.name;
    m.shading = static_cast<ml::FuseMatShading>(c.shading);
    m.category = static_cast<ml::FuseMatCategory>(c.category);
    m.wind = static_cast<ml::FuseMatWind>(c.wind);
    for (u32 i = 0; i < 3u; ++i) {
        m.albedo[i] = c.albedo[i];
    }
    m.roughness = c.roughness;
    m.metallic = c.metallic;
    m.normalStrength = c.normal_strength;
    m.textures.albedo = c.textures.albedo;
    m.textures.normal = c.textures.normal;
    m.uvScale = c.uv_scale;
    m.triplanar = c.triplanar;
    m.triplanarSharpness = c.triplanar_sharpness;
    m.stochastic = c.stochastic;
    m.stochasticLattice = c.stochastic_lattice;
    m.macroScale = c.macro_scale;
    m.macroStrength = c.macro_strength;
    m.detail.albedo = c.detail.albedo;
    m.detail.normal = c.detail.normal;
    m.detailScale = c.detail_scale;
    m.detailStrength = c.detail_strength;
    m.detailFade[0] = c.detail_fade[0];
    m.detailFade[1] = c.detail_fade[1];
    for (const asset::CookedMaterialLayer& l : c.layers) {
        ml::FuseMatLayer o{};
        o.name = l.name;
        o.mode = static_cast<ml::MlLayerMode>(l.mode);
        o.mask = static_cast<ml::MlMask>(l.mask);
        o.maskBias = l.mask_bias;
        o.maskScale = l.mask_scale;
        o.coverage = l.coverage;
        o.contrast = l.contrast;
        for (u32 i = 0; i < 3u; ++i) {
            o.albedo[i] = l.albedo[i];
        }
        o.roughness = l.roughness;
        o.metallic = l.metallic;
        o.uvScale = l.uv_scale;
        o.normalStrength = l.normal_strength;
        o.textures.albedo = l.textures.albedo;
        o.textures.normal = l.textures.normal;
        m.layers.push_back(std::move(o));
    }
    m.proceduralFunction = c.procedural_function;
    m.proceduralParams = c.procedural_params;
    return m;
}

u32 gpu_shading_model(ml::FuseMatShading shading) {
    switch (shading) {
    case ml::FuseMatShading::DefaultLit:
        return static_cast<u32>(ShadingModel::Opaque);
    case ml::FuseMatShading::Subsurface:
    case ml::FuseMatShading::Foliage:
        return static_cast<u32>(ShadingModel::SubsurfaceSSS);
    case ml::FuseMatShading::ClearCoat:
        return static_cast<u32>(ShadingModel::ClearCoat);
    case ml::FuseMatShading::Cloth:
        return static_cast<u32>(ShadingModel::Cloth);
    case ml::FuseMatShading::Unlit:
        return static_cast<u32>(ShadingModel::Emissive);
    case ml::FuseMatShading::Count:
        break;
    }
    return static_cast<u32>(ShadingModel::Opaque);
}

// ================================================================================================================
CookedAssetRegistry::~CookedAssetRegistry() { destroy(); }

bool CookedAssetRegistry::init(const CookedAssetRegistryDesc& desc) {
    destroy();
    if (desc.scene == nullptr || desc.meshes == nullptr || desc.materialCapacity == 0u) {
        return false;
    }
    if (desc.device != nullptr && (desc.allocator == nullptr || desc.bindless == nullptr || desc.upload == nullptr)) {
        return false;
    }
    m_desc = desc;
    m_initialized = true;
    m_dirty = true;
    m_retired.reserve(16);
    return true;
}

bool CookedAssetRegistry::init(scene_renderer::SceneRenderer& renderer, CookedAssetRegistryDesc desc) {
    desc.meshes = &renderer.meshes();
    desc.scene = &renderer.scene();
    desc.composer = &renderer.composer();
    desc.upload = &renderer.upload();
    return init(desc);
}

void CookedAssetRegistry::destroy() {
    if (!m_initialized) {
        return;
    }
    if (m_desc.composer != nullptr) {
        m_desc.composer->setLayeredMaterials(0u, rg::ImportedBuffer{});
    }
    for (TextureEntry& t : m_textures) {
        if (t.slot.isValid() && m_desc.bindless != nullptr) {
            m_desc.bindless->freeSlot(t.slot);
        }
        if (t.image.image != nullptr && m_desc.allocator != nullptr) {
            m_desc.allocator->destroyImage(t.image);
        }
    }
    if (m_tableSlot.isValid() && m_desc.bindless != nullptr) {
        m_desc.bindless->freeSlot(m_tableSlot);
    }
    if (m_table.handle != nullptr && m_desc.allocator != nullptr) {
        m_desc.allocator->destroyBuffer(m_table);
    }
    collectRetired(~0ull);
    m_textures.clear();
    m_tableTextures.clear();
    m_textureIds.clear();
    m_textureAssets.clear();
    m_materials.clear();
    m_materialAssets.clear();
    m_meshes.clear();
    m_table = Buffer{};
    m_tableSlot = BindlessSlotHandle{};
    m_tableHandle = 0;
    m_tableQueue = rg::kNoQueue;
    m_stats = CookedAssetStats{};
    m_initialized = false;
}

u64 CookedAssetRegistry::retireSerial() const { return m_desc.bindless != nullptr ? m_desc.bindless->frameSerial() : 0u; }

void CookedAssetRegistry::retireImage(Texture& image, BindlessSlotHandle& slot) {
    if (slot.isValid() && m_desc.bindless != nullptr) {
        m_desc.bindless->retireSlot(slot, retireSerial());
    }
    slot = BindlessSlotHandle{};
    if (image.image != nullptr) {
        Retired r{};
        r.image = image;
        r.serial = retireSerial();
        m_retired.push_back(r);
    }
    image = Texture{};
}

void CookedAssetRegistry::retireTable() {
    if (m_tableSlot.isValid() && m_desc.bindless != nullptr) {
        m_desc.bindless->retireSlot(m_tableSlot, retireSerial());
    }
    m_tableSlot = BindlessSlotHandle{};
    m_tableHandle = 0;
    if (m_table.handle != nullptr) {
        Retired r{};
        r.buffer = m_table;
        r.serial = retireSerial();
        m_retired.push_back(r);
    }
    m_table = Buffer{};
    m_tableQueue = rg::kNoQueue;
}

u32 CookedAssetRegistry::collectRetired(u64 completedSerial) {
    u32 collected = 0;
    usize keep = 0;
    for (usize i = 0; i < m_retired.size(); ++i) {
        if (m_retired[i].serial > completedSerial) {
            m_retired[keep++] = m_retired[i];
            continue;
        }
        if (m_desc.allocator != nullptr) {
            if (m_retired[i].image.image != nullptr) {
                m_desc.allocator->destroyImage(m_retired[i].image);
            }
            if (m_retired[i].buffer.handle != nullptr) {
                m_desc.allocator->destroyBuffer(m_retired[i].buffer);
            }
        }
        ++collected;
    }
    m_retired.resize(keep);
    return collected;
}

// --- textures ------------------------------------------------------------------------------------------------
u32 CookedAssetRegistry::createTexture(std::string_view primaryId, asset::AssetId asset, const asset::CookedTexture* cooked,
                                       const std::vector<u32>* rgba8, u32 width, u32 height, bool srgb,
                                       std::string* error) {
    if (!m_initialized) {
        setError(error, "cooked assets: registry not initialised");
        return ml::kMlNoTexture;
    }
    // Level 0 decoded (table row mean; the CPU-only mode needs nothing else).
    std::vector<u32> level0;
    bool srgbFlag = srgb;
    bool rgOnly = false; // BC5: B / A read as 1 (ml_tex_channels)
    if (cooked != nullptr) {
        if (cooked->levels.empty() || !decode_cooked_level_rgba8(*cooked, 0u, 0u, level0)) {
            setError(error, "cooked assets: texture cannot be decoded");
            return ml::kMlNoTexture;
        }
        width = cooked->width;
        height = cooked->height;
        const bool colour = cooked->format == asset::BcFormat::BC1 || cooked->format == asset::BcFormat::BC7;
        srgbFlag = colour && cooked->srgb;
        rgOnly = cooked->format == asset::BcFormat::BC5;
    } else {
        if (rgba8 == nullptr || width == 0u || height == 0u || rgba8->size() < static_cast<usize>(width) * height) {
            setError(error, "cooked assets: RGBA8 texture has no texels");
            return ml::kMlNoTexture;
        }
        level0.assign(rgba8->begin(), rgba8->begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(width) * height));
    }

    TextureEntry entry{};
    entry.live = true;
    entry.asset = asset;
    if (m_desc.device != nullptr) {
        if (cooked != nullptr) {
            CookedTextureUploadDesc ud{};
            ud.device = m_desc.device;
            ud.allocator = m_desc.allocator;
            ud.upload = m_desc.upload;
            ud.forceCpuDecode = m_desc.forceCpuBcDecode;
            ud.name = "cooked.texture";
            CookedTextureGpu gpu{};
            std::string why;
            if (!upload_cooked_texture(ud, *cooked, gpu, &why)) {
                setError(error, why);
                return ml::kMlNoTexture;
            }
            entry.image = gpu.image;
            entry.native = gpu.path == CookedTexturePath::Native;
            entry.bytes = gpu.stagedBytes;
        } else {
            // The box-filtered chain of ml_build_mips (the uncooked twin of a cooked texture).
            ml::MlLibrary lib;
            lib.addTexture(primaryId, width, height, srgbFlag ? static_cast<u32>(ml::kMlTexSrgb) : 0u, level0);
            ml::MlMipChains chains;
            ml::ml_build_mips(lib, chains);
            TextureDesc td{};
            td.width = width;
            td.height = height;
            td.mipLevels = chains.levelCount[0];
            td.format = srgbFlag ? GpuFormat::R8G8B8A8Srgb : GpuFormat::R8G8B8A8Unorm;
            td.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
            td.name = "cooked.texture_rgba8";
            UploadImageDesc ud{};
            ud.width = width;
            ud.height = height;
            ud.mipLevels = td.mipLevels;
            ud.bytesPerTexel = 4u;
            if (UploadQueue::imageStagingBytes(ud) > m_desc.upload->ringCapacity()) {
                setError(error, "cooked assets: texture larger than the staging ring");
                return ml::kMlNoTexture;
            }
            if (!m_desc.allocator->createImage(td, entry.image)) {
                setError(error, "cooked assets: image creation failed");
                return ml::kMlNoTexture;
            }
            usize offset = 0;
            if (!m_desc.upload->stageImage(chains.texels.data(), ud, offset) ||
                !m_desc.upload->recordImageCopy(entry.image.image, offset, ud)) {
                m_desc.allocator->destroyImage(entry.image);
                setError(error, "cooked assets: staging failed");
                return ml::kMlNoTexture;
            }
            entry.bytes = chains.textureTexels(0) * 4u;
        }
        entry.slot = m_desc.bindless->registerTextureSlot(entry.image, false);
        entry.handle = m_desc.bindless->shaderHandle(entry.slot);
        if (entry.handle == kBindlessInvalidShaderHandle) {
            // The copy is recorded: retire the image behind the frame serial instead of destroying it now.
            entry.handle = 0;
            retireImage(entry.image, entry.slot);
            setError(error, "cooked assets: no bindless slot");
            return ml::kMlNoTexture;
        }
    }

    ml::MlTexture row{};
    row.offset = entry.handle;
    row.width = width;
    row.height = height;
    row.flags = (srgbFlag ? static_cast<u32>(ml::kMlTexSrgb) : 0u) | (rgOnly ? static_cast<u32>(ml::kMlTexRg) : 0u);
    textureMean(level0, srgbFlag, row.mean);
    if (rgOnly) {
        row.mean[2] = 1.f; // what ml_tex_channels returns
        row.mean[3] = 1.f;
    }

    u32 index = 0;
    while (index < m_textures.size() && m_textures[index].live) {
        ++index;
    }
    if (index == m_textures.size()) {
        m_textures.emplace_back();
        m_tableTextures.emplace_back();
    }
    for (std::string& id : cookIdsOf(primaryId)) {
        entry.ids.push_back(id);
        m_textureIds[id] = index;
    }
    m_textures[index] = std::move(entry);
    m_tableTextures[index] = row;
    if (asset.valid()) {
        m_textureAssets[asset.value] = index;
    }
    ++m_stats.textures;
    m_stats.nativeBcTextures += m_textures[index].native ? 1u : 0u;
    m_stats.textureBytes += m_textures[index].bytes;
    m_dirty = true;
    resolveAll();
    return index;
}

void CookedAssetRegistry::releaseTexture(u32 index) {
    if (index >= m_textures.size() || !m_textures[index].live) {
        return;
    }
    TextureEntry& t = m_textures[index];
    for (const std::string& id : t.ids) {
        const auto it = m_textureIds.find(id);
        if (it != m_textureIds.end() && it->second == index) {
            m_textureIds.erase(it);
        }
    }
    if (t.asset.valid()) {
        m_textureAssets.erase(t.asset.value);
    }
    m_stats.textures -= 1u;
    m_stats.nativeBcTextures -= t.native ? 1u : 0u;
    m_stats.textureBytes -= t.bytes;
    retireImage(t.image, t.slot);
    t = TextureEntry{};
    m_tableTextures[index] = ml::MlTexture{};
    m_dirty = true;
    resolveAll();
}

u32 CookedAssetRegistry::addRgba8Texture(std::string_view id, u32 width, u32 height, bool srgb,
                                         const std::vector<u32>& texels) {
    const u32 index = createTexture(id, asset::AssetId{}, nullptr, &texels, width, height, srgb, nullptr);
    m_stats.failures += index == ml::kMlNoTexture ? 1u : 0u;
    return index;
}

u32 CookedAssetRegistry::addCookedTexture(std::string_view id, const asset::CookedTexture& texture) {
    const u32 index = createTexture(id, asset::AssetId{}, &texture, nullptr, 0u, 0u, texture.srgb, nullptr);
    m_stats.failures += index == ml::kMlNoTexture ? 1u : 0u;
    return index;
}

bool CookedAssetRegistry::addTextureAlias(std::string_view id, u32 textureIndex) {
    if (id.empty() || textureIndex >= m_textures.size() || !m_textures[textureIndex].live) {
        return false;
    }
    m_textures[textureIndex].ids.emplace_back(id);
    m_textureIds[std::string(id)] = textureIndex;
    resolveAll();
    return true;
}

u32 CookedAssetRegistry::findTexture(std::string_view id) const {
    const auto it = m_textureIds.find(std::string(id));
    return it != m_textureIds.end() ? it->second : ml::kMlNoTexture;
}

u32 CookedAssetRegistry::textureHandle(u32 textureIndex) const {
    return textureIndex < m_textures.size() && m_textures[textureIndex].live ? m_textures[textureIndex].handle : 0u;
}

bool CookedAssetRegistry::textureIsNative(u32 textureIndex) const {
    return textureIndex < m_textures.size() && m_textures[textureIndex].live && m_textures[textureIndex].native;
}

const Texture* CookedAssetRegistry::textureImage(u32 textureIndex) const {
    return textureIndex < m_textures.size() && m_textures[textureIndex].live ? &m_textures[textureIndex].image : nullptr;
}

// --- materials -------------------------------------------------------------------------------------------------
void CookedAssetRegistry::resolveMaterial(MaterialEntry& entry) {
    // Unknown ids resolve to "no texture" (counted) instead of failing: the texture may still be loading.
    u32 unresolved = 0;
    auto resolver = [&](std::string_view id) -> u32 {
        const u32 t = findTexture(id);
        if (t == ml::kMlNoTexture) {
            ++unresolved;
            return 0xFFFFFFFEu; // placeholder, cleared below
        }
        return t;
    };
    ml::MlMaterial out{};
    const ml::FuseMatResult r = ml::resolve_fusemat(entry.source, resolver, out);
    if (!r.ok) {
        out = ml::MlMaterial{};
    }
    auto clear = [](u32& slot) {
        if (slot == 0xFFFFFFFEu) {
            slot = ml::kMlNoTexture;
        }
    };
    clear(out.albedoTex);
    clear(out.normalTex);
    clear(out.detailAlbedoTex);
    clear(out.detailNormalTex);
    for (u32 i = 0; i < ml::kMlMaxLayers; ++i) {
        clear(out.layers[i].albedoTex);
        clear(out.layers[i].normalTex);
    }
    if (std::memcmp(&out, &entry.resolved, sizeof(out)) != 0) {
        m_dirty = true;
    }
    entry.resolved = out;
    entry.unresolved = unresolved;
}

void CookedAssetRegistry::resolveAll() {
    u32 unresolved = 0;
    for (MaterialEntry& m : m_materials) {
        if (m.live) {
            resolveMaterial(m);
            unresolved += m.unresolved;
        }
    }
    m_stats.unresolvedSlots = unresolved;
}

void CookedAssetRegistry::writeMaterialRow(const MaterialEntry& entry, u32 tableIndex) {
    const ml::FuseMat& f = entry.source;
    Material m{};
    m.baseColor = fuse::math::Vec3{f.albedo[0], f.albedo[1], f.albedo[2]};
    m.roughness = f.roughness;
    m.metallic = f.metallic;
    m.normalStrength = f.normalStrength;
    m.shadingModel = static_cast<ShadingModel>(gpu_shading_model(f.shading));
    if (f.shading == ml::FuseMatShading::Unlit) {
        m.emissiveColor = m.baseColor;
        m.emissiveIntensity = 1.f;
    }
    if (f.shading == ml::FuseMatShading::ClearCoat) {
        m.parameters.clearCoat.clearCoat = 1.f;
    }
    gpu_scene::GpuMaterial row = m.pack();
    gpu_scene::set_gpu_material_layered(row, tableIndex);
    m_desc.scene->setMaterial(entry.row, row);
}

u32 CookedAssetRegistry::createMaterial(const ml::FuseMat& material, asset::AssetId asset, std::string* error) {
    if (!m_initialized) {
        setError(error, "cooked assets: registry not initialised");
        return kInvalid;
    }
    const ml::FuseMatResult v = ml::validate_fusemat(material);
    if (!v.ok) {
        setError(error, "cooked assets: invalid material: " + v.describe());
        return kInvalid;
    }
    u32 index = 0;
    while (index < m_materials.size() && m_materials[index].live) {
        ++index;
    }
    if (index >= m_desc.materialCapacity) {
        setError(error, "cooked assets: material rows exhausted");
        return kInvalid;
    }
    if (index == m_materials.size()) {
        m_materials.emplace_back();
    }
    MaterialEntry& e = m_materials[index];
    e = MaterialEntry{};
    e.live = true;
    e.asset = asset;
    e.source = material;
    e.row = m_desc.firstMaterialRow + index;
    resolveMaterial(e);
    writeMaterialRow(e, index);
    if (asset.valid()) {
        m_materialAssets[asset.value] = index;
    }
    ++m_stats.materials;
    m_dirty = true;
    resolveAll();
    return e.row;
}

void CookedAssetRegistry::releaseMaterial(u32 index) {
    if (index >= m_materials.size() || !m_materials[index].live) {
        return;
    }
    MaterialEntry& e = m_materials[index];
    m_desc.scene->setMaterial(e.row, Material{}.pack());
    if (e.asset.valid()) {
        m_materialAssets.erase(e.asset.value);
    }
    e = MaterialEntry{};
    m_stats.materials -= 1u;
    m_dirty = true;
    resolveAll();
}

u32 CookedAssetRegistry::addMaterial(const ml::FuseMat& material, std::string* error) {
    const u32 row = createMaterial(material, asset::AssetId{}, error);
    m_stats.failures += row == kInvalid ? 1u : 0u;
    return row;
}

u32 CookedAssetRegistry::materialRow(asset::AssetId id) const {
    const auto it = m_materialAssets.find(id.value);
    return it != m_materialAssets.end() ? m_materials[it->second].row : kInvalid;
}

u32 CookedAssetRegistry::materialTableIndex(u32 row) const {
    if (row < m_desc.firstMaterialRow) {
        return kInvalid;
    }
    const u32 index = row - m_desc.firstMaterialRow;
    return index < m_materials.size() && m_materials[index].live ? index : kInvalid;
}

const ml::MlMaterial* CookedAssetRegistry::tableMaterial(u32 tableIndex) const {
    return tableIndex < m_materials.size() && m_materials[tableIndex].live ? &m_materials[tableIndex].resolved : nullptr;
}

std::vector<ml::MlMaterial> CookedAssetRegistry::tableMaterials() const {
    std::vector<ml::MlMaterial> rows(m_materials.size());
    for (usize i = 0; i < m_materials.size(); ++i) {
        rows[i] = m_materials[i].live ? m_materials[i].resolved : ml::MlMaterial{};
    }
    return rows;
}

// --- meshes ----------------------------------------------------------------------------------------------------
u32 CookedAssetRegistry::addMesh(asset::AssetId assetId, const asset::CookedMesh& mesh, std::string* error) {
    if (!m_initialized || !assetId.valid()) {
        setError(error, "cooked assets: registry not initialised / invalid asset id");
        ++m_stats.failures;
        return kInvalid;
    }
    auto data = std::make_unique<CookedMeshletResult>();
    if (!cooked_mesh_to_meshlets(mesh, *data, error)) {
        ++m_stats.failures;
        return kInvalid;
    }
    const u32 engineId = m_desc.meshes->engineIdForAsset(assetId.value);
    if (engineId == scene_renderer::MeshRegistry::kInvalidMesh ||
        !m_desc.meshes->registerMesh(engineId, data->mesh)) {
        setError(error, "cooked assets: mesh registry refused the mesh");
        ++m_stats.failures;
        return kInvalid;
    }
    auto it = m_meshes.find(assetId.value);
    if (it == m_meshes.end()) {
        ++m_stats.meshes;
    } else {
        m_stats.adoptedMeshes -= it->second.data->path == CookedMeshPath::Adopted ? 1u : 0u;
        m_stats.dagMeshes -= it->second.data->hasDag ? 1u : 0u;
    }
    m_stats.adoptedMeshes += data->path == CookedMeshPath::Adopted ? 1u : 0u;
    m_stats.dagMeshes += data->hasDag ? 1u : 0u;
    MeshEntry& e = m_meshes[assetId.value];
    e.asset = assetId;
    e.engineId = engineId;
    e.data = std::move(data);
    return engineId;
}

u32 CookedAssetRegistry::meshEngineId(asset::AssetId id) const {
    const auto it = m_meshes.find(id.value);
    return it != m_meshes.end() ? it->second.engineId : kInvalid;
}

const CookedMeshletResult* CookedAssetRegistry::meshData(asset::AssetId id) const {
    const auto it = m_meshes.find(id.value);
    return it != m_meshes.end() ? it->second.data.get() : nullptr;
}

// --- IRenderUploadSink -------------------------------------------------------------------------------------------
asset::RenderUploadResult CookedAssetRegistry::upload(const asset::RenderUploadCommand& command) {
    asset::RenderUploadResult result{};
    if (!m_initialized || command.payload == nullptr) {
        result.error = "cooked assets: registry not initialised / no payload";
        ++m_stats.failures;
        return result;
    }
    std::string error;
    switch (command.type) {
    case asset::AssetType::Texture: {
        const asset::CookedTexture* t = command.payload->texture();
        if (t == nullptr) {
            break;
        }
        const auto old = m_textureAssets.find(command.id.value);
        if (old != m_textureAssets.end()) {
            releaseTexture(old->second); // re-upload of the same asset replaces it
        }
        const u32 index = createTexture(command.virtual_path, command.id, t, nullptr, 0u, 0u, t->srgb, &error);
        if (index != ml::kMlNoTexture) {
            result.ok = true;
            result.gpu_resource = static_cast<u64>(index) + 1u;
        }
        break;
    }
    case asset::AssetType::Material: {
        const asset::CookedMaterial* m = command.payload->material();
        if (m == nullptr) {
            break;
        }
        const auto old = m_materialAssets.find(command.id.value);
        if (old != m_materialAssets.end()) {
            releaseMaterial(old->second);
        }
        const u32 row = createMaterial(fusemat_from_cooked(*m), command.id, &error);
        if (row != kInvalid) {
            result.ok = true;
            result.gpu_resource = static_cast<u64>(row) + 1u;
        }
        break;
    }
    case asset::AssetType::Mesh: {
        const asset::CookedMesh* m = command.payload->mesh();
        if (m == nullptr) {
            break;
        }
        const u32 engineId = addMesh(command.id, *m, &error);
        if (engineId != kInvalid) {
            result.ok = true;
            result.gpu_resource = engineId;
        } else {
            --m_stats.failures; // counted once below
        }
        break;
    }
    case asset::AssetType::Unknown:
        break;
    }
    if (result.ok) {
        ++m_stats.uploads;
    } else {
        ++m_stats.failures;
        result.error = error.empty() ? std::string("cooked assets: payload does not match the asset type") : error;
    }
    return result;
}

void CookedAssetRegistry::release(const asset::RenderUploadCommand& command) {
    if (!m_initialized) {
        return;
    }
    ++m_stats.releases;
    switch (command.type) {
    case asset::AssetType::Texture: {
        const auto it = m_textureAssets.find(command.id.value);
        if (it != m_textureAssets.end()) {
            releaseTexture(it->second);
        }
        break;
    }
    case asset::AssetType::Material: {
        const auto it = m_materialAssets.find(command.id.value);
        if (it != m_materialAssets.end()) {
            releaseMaterial(it->second);
        }
        break;
    }
    case asset::AssetType::Mesh: {
        // GpuScene has no mesh removal (MeshRegistry): the row stays allocated; entities stop referencing it.
        const auto it = m_meshes.find(command.id.value);
        if (it != m_meshes.end()) {
            m_stats.adoptedMeshes -= it->second.data->path == CookedMeshPath::Adopted ? 1u : 0u;
            m_stats.dagMeshes -= it->second.data->hasDag ? 1u : 0u;
            m_stats.meshes -= 1u;
            m_meshes.erase(it);
        }
        break;
    }
    case asset::AssetType::Unknown:
        break;
    }
}

// --- table -------------------------------------------------------------------------------------------------------
bool CookedAssetRegistry::commit() {
    if (!m_initialized) {
        return false;
    }
    if (!m_dirty) {
        return true;
    }
    for (u32 i = 0; i < static_cast<u32>(m_materials.size()); ++i) {
        if (m_materials[i].live) {
            writeMaterialRow(m_materials[i], i);
        }
    }
    m_dirty = false;
    if (m_desc.device == nullptr) {
        return true;
    }
    retireTable();
    const u32 mc = static_cast<u32>(m_materials.size());
    const u32 tc = static_cast<u32>(m_tableTextures.size());
    const u64 materialsOffset = align256(sizeof(ml::MlResolveTable));
    const u64 texturesOffset = align256(materialsOffset + std::max<u64>(u64{mc} * sizeof(ml::MlMaterial), 16u));
    const u64 bytes = align256(texturesOffset + std::max<u64>(u64{tc} * sizeof(ml::MlTexture), 16u));
    BufferDesc d{};
    d.size = static_cast<usize>(bytes);
    d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                       static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    d.memoryUsage = MemoryUsage::CpuToGpu;
    d.name = "cooked.material_table";
    if (!m_desc.allocator->createBuffer(d, m_table) || m_table.mapped == nullptr || m_table.deviceAddress == 0u) {
        if (m_table.handle != nullptr) {
            m_desc.allocator->destroyBuffer(m_table);
        }
        m_table = Buffer{};
        m_dirty = true;
        return false;
    }
    u8* base = static_cast<u8*>(m_table.mapped);
    std::memset(base, 0, static_cast<usize>(bytes));
    ml::MlResolveTable header{};
    header.materials = m_table.deviceAddress + materialsOffset;
    header.textures = m_table.deviceAddress + texturesOffset;
    header.materialCount = mc;
    header.textureCount = tc;
    std::memcpy(base, &header, sizeof(header));
    const std::vector<ml::MlMaterial> rows = tableMaterials();
    if (mc > 0u) {
        std::memcpy(base + materialsOffset, rows.data(), u64{mc} * sizeof(ml::MlMaterial));
    }
    if (tc > 0u) {
        std::memcpy(base + texturesOffset, m_tableTextures.data(), u64{tc} * sizeof(ml::MlTexture));
    }
    m_tableSlot = m_desc.bindless->registerBufferSlot(m_table, false);
    m_tableHandle = m_desc.bindless->shaderHandle(m_tableSlot);
    if (m_tableHandle == kBindlessInvalidShaderHandle) {
        m_tableHandle = 0;
        retireTable();
        m_dirty = true;
        return false;
    }
    ++m_stats.tableRebuilds;
    if (m_desc.composer != nullptr) {
        m_desc.composer->setLayeredMaterials(
            m_tableHandle, rg::ImportedBuffer{m_table.handle, m_table.desc.size, m_tableQueue, &m_tableQueue,
                                              "cooked.material_table"});
    }
    return true;
}

// --- ECS -----------------------------------------------------------------------------------------------------------
u32 CookedAssetRegistry::syncEntities(ecs::Registry& registry) {
    if (!m_initialized) {
        return 0u;
    }
    u32 changed = 0;
    registry.each<ecs::MeshAssets, ecs::Mesh>([&](ecs::EntityID, ecs::MeshAssets& assets, ecs::Mesh& mesh) {
        bool dirty = false;
        if (assets.mesh.valid()) {
            const auto it = m_meshes.find(assets.mesh.value);
            const ecs::MeshVertexBufferHandle want =
                it != m_meshes.end() ? ecs::MeshVertexBufferHandle(it->second.engineId, 1u) : ecs::MeshVertexBufferHandle{};
            if (mesh.vertex_buffer != want) {
                mesh.vertex_buffer = want;
                dirty = true;
            }
            if (it != m_meshes.end()) {
                const bool empty = !(mesh.aabb_max.x > mesh.aabb_min.x || mesh.aabb_max.y > mesh.aabb_min.y ||
                                     mesh.aabb_max.z > mesh.aabb_min.z);
                if (empty) {
                    const CookedMeshletResult& d = *it->second.data;
                    mesh.aabb_min = ecs::vec3{d.boundsMin[0], d.boundsMin[1], d.boundsMin[2], 0.f};
                    mesh.aabb_max = ecs::vec3{d.boundsMax[0], d.boundsMax[1], d.boundsMax[2], 0.f};
                    dirty = true;
                }
            }
        }
        if (assets.material.valid()) {
            const u32 row = materialRow(assets.material);
            if (row != kInvalid && mesh.material_id != row) {
                mesh.material_id = row;
                dirty = true;
            }
        }
        changed += dirty ? 1u : 0u;
    });
    return changed;
}

} // namespace fuse::renderer::cooked_assets
