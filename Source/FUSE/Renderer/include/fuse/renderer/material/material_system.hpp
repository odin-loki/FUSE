#pragma once

#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Bindless material table (B5.3): authoring materials -> std430 SSBO rows indexed by material id.
/// flushGpuBuffer() repacks dirty rows, resolves texture handles to bindless heap slots and uploads the
/// table (growing the SSBO when materials were added).
///
/// Change tracking for other consumers (E02 SceneRenderer material feed): every registerMaterial /
/// updateMaterial bumps the row's version() and the table's changeSerial(); a consumer that remembers
/// the versions it copied re-reads only rows whose version moved (gpu_scene::GpuSceneMaterialFeed feeds
/// the WP-1.1 GpuScene material table this way). gpuRow() is the row flushGpuBuffer() would upload.
class MaterialSystem {
public:
    void init(ResourceManager& resources);
    /// No ResourceManager: rows, versions and gpuRow() work; flushGpuBuffer() creates no SSBO and texture
    /// handles resolve through Material::pack() (TextureHandle slot or "no texture").
    void initStandalone();
    void destroy();

    bool isReady() const { return m_ready; }

    u32 registerMaterial(const Material& material);
    void updateMaterial(u32 id, const Material& material);
    Material& get(u32 id);
    const Material& get(u32 id) const;
    u32 materialCount() const { return static_cast<u32>(m_materials.size()); }

    void flushGpuBuffer();

    /// Packed row with texture handles resolved to bindless heap slots (what flushGpuBuffer() uploads).
    Material::GPUMaterial gpuRow(u32 id) const;
    /// Starts at 1 on registerMaterial, +1 per updateMaterial (0 for an unknown id).
    u32 version(u32 id) const { return id < m_versions.size() ? m_versions[id] : 0u; }
    /// +1 for every registerMaterial / updateMaterial (0 = nothing registered yet).
    u64 changeSerial() const { return m_changeSerial; }

    const std::vector<Material::GPUMaterial>& gpuMaterials() const { return m_gpuMaterials; }
    BufferHandle materialSsbo() const { return m_materialSsbo; }
    u32 dirtyCount() const { return m_dirtyCount; }

private:
    void ensureCapacity(u32 id);
    void markDirty(u32 id);
    void resolveBindlessIndices(const Material& material, Material::GPUMaterial& gpu) const;

    ResourceManager* m_resources = nullptr;
    std::vector<Material> m_materials;
    std::vector<Material::GPUMaterial> m_gpuMaterials;
    std::vector<bool> m_dirty;
    std::vector<u32> m_versions;
    u64 m_changeSerial = 0;
    BufferHandle m_materialSsbo{};
    u32 m_dirtyCount = 0;
    bool m_ready = false;
};

} // namespace fuse::renderer
