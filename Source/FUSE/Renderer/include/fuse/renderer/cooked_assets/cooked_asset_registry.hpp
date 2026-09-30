#pragma once

// E06 (AP-RT-COOKED, UNI-U7-ASSET-1 render half): the renderer's side of the asset pipeline. CookedAssetRegistry is
// the asset::IRenderUploadSink that asset::AssetRegistry::drainRenderUploads calls on the render thread:
//
//   Texture  (.fusetex)  -> sampled image, VK_FORMAT_BC* or CPU-decoded (cooked_texture_gpu.hpp), registered in
//                           bindless; a row of the registry's layered-material texture table (MlTexture, offset =
//                           bindless handle). Cook ids: the texture's virtual path, that path without extension and
//                           the bare file stem ("game:/tex/rock_albedo.fusetex" -> "rock_albedo"), plus aliases.
//   Material (.fusemat)  -> MlMaterial (material_layers::resolve_fusemat, texture slots through the cook-id table) in
//                           the layered-material table + a GpuScene material row flagged kGpuMaterialLayered whose
//                           padding is the table index (base colour / roughness / metallic / shading model / emissive
//                           mirror the .fusemat for non-layered consumers). Texture ids that are not loaded yet
//                           resolve to "no texture" and are re-resolved when the texture arrives.
//   Mesh     (.fusemesh) -> WP-1.2 meshlet mesh (cooked_mesh.hpp: FMSH v2 meshlets + DAG adopted, else built) queued
//                           on the SceneRenderer's MeshRegistry under MeshRegistry::engineIdForAsset(asset id), so
//                           gpu_scene::GpuSceneEcsExtractor's meshRemap maps ecs::Mesh::vertex_buffer.index() (the
//                           engine id, written by syncEntities from ecs::MeshAssets) to the GpuScene mesh row.
//
// upload() returns a non-zero gpu_resource (texture: table index + 1; material: GpuScene row + 1; mesh: engine id);
// release() frees the GPU copy (image + bindless slot retired at the bindless frame serial, material row reset to the
// default material, mesh engine id unmapped).
//
// The layered-material table buffer ("cooked.material_table": MlResolveTable header + MlMaterial rows + MlTexture
// rows, host visible, bindless storage buffer) is rebuilt by commit() when anything changed (a new buffer; the old one
// retires), and handed to the FrameComposer (setLayeredMaterials) so the material resolve's layered bin samples the
// cooked textures. Frame protocol (render thread):
//
//   assets.drainRenderUploads(registry);   // IRenderUploadSink::upload / release
//   registry.commit();                     // table rebuild when dirty (+ composer hand-off)
//   registry.syncEntities(ecs);            // ecs::MeshAssets -> ecs::Mesh engine id / material row / bounds
//   upload.flush() happens in SceneRenderer::renderScene (images recorded on its UploadQueue)
//   renderer.renderScene(ecs, camera, graph); ... registry.collectRetired(completedSerial);
//
// Without a device (desc.device null, CPU-only GpuScene) textures get no image but keep their table rows (handle 0),
// so the CPU gates exercise resolution, rows and ECS sync. Not thread-safe: one render thread.

#include <fuse/asset/asset_id.hpp>
#include <fuse/asset/render_upload.hpp>
#include <fuse/renderer/cooked_assets/cooked_mesh.hpp>
#include <fuse/renderer/cooked_assets/cooked_texture_gpu.hpp>
#include <fuse/renderer/material_layers/fusemat.hpp>
#include <fuse/renderer/material_layers/ml_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/rg_types.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fuse::ecs {
class Registry;
}

namespace fuse::renderer {
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
namespace frame {
class FrameComposer;
}
namespace gpu_scene {
class GpuScene;
}
namespace scene_renderer {
class MeshRegistry;
class SceneRenderer;
} // namespace scene_renderer
} // namespace fuse::renderer

namespace fuse::renderer::cooked_assets {

/// .fusemat runtime record (fuse::asset::CookedMaterial, the dependency-free reader's output) -> the renderer's FuseMat
/// (field for field; the enums are the same u32 values).
material_layers::FuseMat fusemat_from_cooked(const asset::CookedMaterial& material);

/// Renderer shading model (GPUMaterial::shadingModel, renderer::ShadingModel) of a .fusemat shading model:
/// default_lit -> Opaque, subsurface / foliage -> SubsurfaceSSS, clear_coat -> ClearCoat, cloth -> Cloth,
/// unlit -> Emissive (the albedo is emitted).
u32 gpu_shading_model(material_layers::FuseMatShading shading);

struct CookedAssetRegistryDesc {
    VulkanDevice* device = nullptr;          ///< null: CPU-only (no images / table buffer)
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    UploadQueue* upload = nullptr;           ///< texture uploads are recorded here (flushed by its owner)
    scene_renderer::MeshRegistry* meshes = nullptr;
    gpu_scene::GpuScene* scene = nullptr;    ///< material rows
    frame::FrameComposer* composer = nullptr; ///< optional: receives the layered table (setLayeredMaterials)
    /// GpuScene material rows owned by the registry: [firstMaterialRow, firstMaterialRow + materialCapacity).
    u32 firstMaterialRow = 128;
    u32 materialCapacity = 128;
    bool forceCpuBcDecode = false; ///< decode BCn on the CPU even when the device samples it (fallback gate)
};

struct CookedAssetStats {
    u32 textures = 0;         ///< live textures
    u32 nativeBcTextures = 0; ///< of which VK_FORMAT_BC* images
    u32 materials = 0;        ///< live materials
    u32 meshes = 0;           ///< live meshes
    u32 adoptedMeshes = 0;    ///< meshes whose FMSH v2 meshlet table was used as cooked
    u32 dagMeshes = 0;        ///< meshes with an adopted cluster DAG
    u32 unresolvedSlots = 0;  ///< material texture slots whose cook id is not loaded
    u64 uploads = 0;
    u64 releases = 0;
    u64 failures = 0;
    u32 tableRebuilds = 0;
    u64 textureBytes = 0;     ///< bytes staged for live textures
};

class CookedAssetRegistry final : public asset::IRenderUploadSink {
public:
    CookedAssetRegistry() = default;
    ~CookedAssetRegistry() override;
    CookedAssetRegistry(const CookedAssetRegistry&) = delete;
    CookedAssetRegistry& operator=(const CookedAssetRegistry&) = delete;

    bool init(const CookedAssetRegistryDesc& desc);
    /// Fills meshes / scene / composer / upload from `renderer` (device, allocator and bindless come from `desc`).
    bool init(scene_renderer::SceneRenderer& renderer, CookedAssetRegistryDesc desc);
    /// Releases every GPU copy at once (the caller must have retired every frame that used them).
    void destroy();
    bool valid() const { return m_initialized; }

    // --- asset::IRenderUploadSink (render thread) --------------------------------------------------------------
    asset::RenderUploadResult upload(const asset::RenderUploadCommand& command) override;
    void release(const asset::RenderUploadCommand& command) override;

    // --- direct content (tools, uncooked twins of cooked assets, tests) ----------------------------------------
    /// An RGBA8 texture (x fastest, width * height words r | g << 8 | b << 16 | a << 24) with the full box-filtered
    /// mip chain of material_layers::ml_build_mips, under cook id `id`. Returns its table index (kMlNoTexture on
    /// failure).
    u32 addRgba8Texture(std::string_view id, u32 width, u32 height, bool srgb, const std::vector<u32>& texels);
    /// A cooked texture that did not come through the AssetRegistry (table index, kMlNoTexture on failure).
    u32 addCookedTexture(std::string_view id, const asset::CookedTexture& texture);
    /// A material resolved against the cook-id table; returns its GpuScene row (kInvalid on failure).
    u32 addMaterial(const material_layers::FuseMat& material, std::string* error = nullptr);
    /// A mesh under engine id MeshRegistry::engineIdForAsset(assetId); returns the engine id (kInvalid on failure).
    u32 addMesh(asset::AssetId assetId, const asset::CookedMesh& mesh, std::string* error = nullptr);
    /// Extra cook id for a texture table index.
    bool addTextureAlias(std::string_view id, u32 textureIndex);

    // --- per frame (render thread) -------------------------------------------------------------------------------
    /// Rebuilds the layered-material table when textures / materials changed and hands it to the composer.
    /// False when the table buffer could not be created.
    bool commit();
    /// ecs::MeshAssets -> ecs::Mesh for every entity that has both: vertex_buffer = the mesh's engine id (invalid
    /// while the mesh is not uploaded), material_id = the material's GpuScene row (unchanged while not uploaded),
    /// aabb from the cooked bounds when the entity's box is empty. Returns the entities whose Mesh changed.
    u32 syncEntities(ecs::Registry& registry);
    /// Destroys images / table buffers retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- queries ---------------------------------------------------------------------------------------------------
    static constexpr u32 kInvalid = 0xFFFFFFFFu;
    /// Table index of a cook id (kMlNoTexture when unknown).
    u32 findTexture(std::string_view id) const;
    /// Bindless sampled-image shader handle of a table index (0 without an image).
    u32 textureHandle(u32 textureIndex) const;
    bool textureIsNative(u32 textureIndex) const;
    const Texture* textureImage(u32 textureIndex) const;
    /// GpuScene material row of a material asset (kInvalid when not uploaded).
    u32 materialRow(asset::AssetId id) const;
    /// Layered-table index of a GpuScene material row (kInvalid when the row is not the registry's).
    u32 materialTableIndex(u32 row) const;
    const material_layers::MlMaterial* tableMaterial(u32 tableIndex) const;
    /// Engine mesh id of a mesh asset (kInvalid when not uploaded).
    u32 meshEngineId(asset::AssetId id) const;
    const CookedMeshletResult* meshData(asset::AssetId id) const;
    /// Table texture rows / material rows as the GPU sees them (offset = bindless handle).
    const std::vector<material_layers::MlTexture>& tableTextures() const { return m_tableTextures; }
    std::vector<material_layers::MlMaterial> tableMaterials() const;
    u32 tableHandle() const { return m_tableHandle; }
    const Buffer& tableBuffer() const { return m_table; }
    bool dirty() const { return m_dirty; }
    const CookedAssetStats& stats() const { return m_stats; }

private:
    struct TextureEntry {
        bool live = false;
        asset::AssetId asset{};
        Texture image{};
        BindlessSlotHandle slot{};
        u32 handle = 0;
        bool native = false;
        u64 bytes = 0;
        std::vector<std::string> ids;
    };
    struct MaterialEntry {
        bool live = false;
        asset::AssetId asset{};
        material_layers::FuseMat source;
        material_layers::MlMaterial resolved{};
        u32 row = 0;
        u32 unresolved = 0;
    };
    struct MeshEntry {
        asset::AssetId asset{};
        u32 engineId = 0;
        std::unique_ptr<CookedMeshletResult> data;
    };
    struct Retired {
        Texture image{};
        Buffer buffer{};
        u64 serial = 0;
    };

    u32 createTexture(std::string_view primaryId, asset::AssetId asset, const asset::CookedTexture* cooked,
                      const std::vector<u32>* rgba8, u32 width, u32 height, bool srgb, std::string* error);
    void releaseTexture(u32 index);
    u32 createMaterial(const material_layers::FuseMat& material, asset::AssetId asset, std::string* error);
    void releaseMaterial(u32 index);
    void resolveMaterial(MaterialEntry& entry);
    void writeMaterialRow(const MaterialEntry& entry, u32 tableIndex);
    void resolveAll();
    void retireImage(Texture& image, BindlessSlotHandle& slot);
    void retireTable();
    u64 retireSerial() const;

    CookedAssetRegistryDesc m_desc{};
    bool m_initialized = false;
    bool m_dirty = true;
    std::vector<TextureEntry> m_textures;
    std::vector<material_layers::MlTexture> m_tableTextures;
    std::unordered_map<std::string, u32> m_textureIds;
    std::unordered_map<u64, u32> m_textureAssets; ///< asset id -> texture index
    std::vector<MaterialEntry> m_materials;       ///< index = table index; row = firstMaterialRow + index
    std::unordered_map<u64, u32> m_materialAssets;
    std::unordered_map<u64, MeshEntry> m_meshes;
    std::vector<Retired> m_retired;
    Buffer m_table{};
    BindlessSlotHandle m_tableSlot{};
    u32 m_tableHandle = 0;
    u8 m_tableQueue = rg::kNoQueue;
    CookedAssetStats m_stats{};
};

} // namespace fuse::renderer::cooked_assets
