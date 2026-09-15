#include <fuse/renderer/material/material_system.hpp>

namespace fuse::renderer {

void MaterialSystem::init(ResourceManager& resources) {
    destroy();
    m_resources = &resources;
    m_ready = true;
}

void MaterialSystem::destroy() {
    if (m_resources != nullptr && m_materialSsbo.isValid()) {
        m_resources->destroyBuffer(m_materialSsbo);
    }

    m_resources = nullptr;
    m_materials.clear();
    m_gpuMaterials.clear();
    m_dirty.clear();
    m_materialSsbo = BufferHandle{};
    m_dirtyCount = 0;
    m_ready = false;
}

u32 MaterialSystem::registerMaterial(const Material& material) {
    if (!m_ready) {
        return UINT32_MAX;
    }

    const u32 id = static_cast<u32>(m_materials.size());
    m_materials.push_back(material);
    m_gpuMaterials.push_back(material.pack());
    m_dirty.push_back(true);
    ++m_dirtyCount;
    return id;
}

void MaterialSystem::updateMaterial(u32 id, const Material& material) {
    if (!m_ready || id >= m_materials.size()) {
        return;
    }

    m_materials[id] = material;
    markDirty(id);
}

Material& MaterialSystem::get(u32 id) {
    return m_materials.at(id);
}

const Material& MaterialSystem::get(u32 id) const {
    return m_materials.at(id);
}

void MaterialSystem::flushGpuBuffer() {
    if (!m_ready || m_dirtyCount == 0u) {
        return;
    }

    for (u32 i = 0; i < static_cast<u32>(m_dirty.size()); ++i) {
        if (!m_dirty[i]) {
            continue;
        }
        m_gpuMaterials[i] = m_materials[i].pack();
        m_dirty[i] = false;
    }
    m_dirtyCount = 0;

    if (m_resources == nullptr) {
        return;
    }

    const usize byteSize = m_gpuMaterials.size() * sizeof(Material::GPUMaterial);
    if (byteSize == 0u) {
        return;
    }

    if (!m_materialSsbo.isValid()) {
        BufferDesc desc{};
        desc.size = byteSize;
        desc.usage = BufferUsage::Storage;
        desc.memoryUsage = MemoryUsage::CpuToGpu;
        desc.name = "material_ssbo";
        m_materialSsbo = m_resources->createBuffer(desc);
    }
}

void MaterialSystem::ensureCapacity(u32 /*id*/) {}

void MaterialSystem::markDirty(u32 id) {
    if (id >= m_dirty.size() || m_dirty[id]) {
        return;
    }
    m_dirty[id] = true;
    ++m_dirtyCount;
}

} // namespace fuse::renderer
