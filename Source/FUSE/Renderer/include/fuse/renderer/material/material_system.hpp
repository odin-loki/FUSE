#pragma once

#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Bindless material table scaffold (B5.3).
class MaterialSystem {
public:
    void init(ResourceManager& resources);
    void destroy();

    bool isReady() const { return m_ready; }

    u32 registerMaterial(const Material& material);
    void updateMaterial(u32 id, const Material& material);
    Material& get(u32 id);
    const Material& get(u32 id) const;
    u32 materialCount() const { return static_cast<u32>(m_materials.size()); }

    void flushGpuBuffer();

    const std::vector<Material::GPUMaterial>& gpuMaterials() const { return m_gpuMaterials; }
    BufferHandle materialSsbo() const { return m_materialSsbo; }
    u32 dirtyCount() const { return m_dirtyCount; }

private:
    void ensureCapacity(u32 id);
    void markDirty(u32 id);

    ResourceManager* m_resources = nullptr;
    std::vector<Material> m_materials;
    std::vector<Material::GPUMaterial> m_gpuMaterials;
    std::vector<bool> m_dirty;
    BufferHandle m_materialSsbo{};
    u32 m_dirtyCount = 0;
    bool m_ready = false;
};

} // namespace fuse::renderer
