#include <fuse/renderer/material/material_system.hpp>

#include <cstring>

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
        resolveBindlessIndices(m_materials[i], m_gpuMaterials[i]);
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

    // Rows are rewritten wholesale: a mapped (CpuToGpu) buffer is updated in place; a buffer that is
    // too small (materials registered since the last flush) or unmapped is recreated with the rows as
    // its initial data (its bindless index may change; read it from materialSsbo() each frame).
    Buffer* existing = m_materialSsbo.isValid() ? m_resources->getBuffer(m_materialSsbo) : nullptr;
    if (existing != nullptr && existing->desc.size >= byteSize && existing->mapped != nullptr) {
        std::memcpy(existing->mapped, m_gpuMaterials.data(), byteSize);
        return;
    }
    if (m_materialSsbo.isValid()) {
        m_resources->destroyBuffer(m_materialSsbo);
        m_materialSsbo = BufferHandle{};
    }

    BufferDesc desc{};
    desc.size = byteSize;
    desc.usage = BufferUsage::Storage;
    desc.memoryUsage = MemoryUsage::CpuToGpu;
    desc.name = "material_ssbo";
    m_materialSsbo = m_resources->createBuffer(desc, m_gpuMaterials.data());
}

void MaterialSystem::ensureCapacity(u32 /*id*/) {}

void MaterialSystem::resolveBindlessIndices(const Material& material, Material::GPUMaterial& gpu) const {
    if (m_resources == nullptr) {
        return;
    }
    // Material::pack() can only see handle slots; shaders index the bindless texture heap, whose slot
    // allocator is independent of the handle map. Unknown / destroyed handles resolve to "no texture".
    const auto resolve = [this](const TextureHandle& handle) -> u32 {
        if (!handle.isValid()) {
            return UINT32_MAX;
        }
        const Texture* texture = m_resources->getTexture(handle);
        return texture != nullptr ? texture->bindlessIndex : UINT32_MAX;
    };
    gpu.baseColorTexIdx = resolve(material.baseColorTex);
    gpu.roughnessTexIdx = resolve(material.roughnessTex);
    gpu.metallicTexIdx = resolve(material.metallicTex);
    gpu.normalTexIdx = resolve(material.normalTex);
    gpu.aoTexIdx = resolve(material.aoTex);
    gpu.emissiveTexIdx = resolve(material.emissiveTex);
    if (gpu.normalTexIdx == UINT32_MAX) {
        gpu.flags &= ~MaterialFlagBits::kHasNormalMap;
    }
    if (gpu.aoTexIdx == UINT32_MAX) {
        gpu.flags &= ~MaterialFlagBits::kHasAoMap;
    }
    if (gpu.metallicTexIdx == UINT32_MAX) {
        gpu.flags &= ~MaterialFlagBits::kHasMetallicMap;
    }
}

void MaterialSystem::markDirty(u32 id) {
    if (id >= m_dirty.size() || m_dirty[id]) {
        return;
    }
    m_dirty[id] = true;
    ++m_dirtyCount;
}

} // namespace fuse::renderer
