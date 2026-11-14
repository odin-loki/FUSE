#include <fuse/hybrid/mesh_sdf_preview_stub.hpp>

namespace fuse::hybrid {

void MeshSdfPreviewCatalog::clear() {
    m_meshCount = 0;
    m_sdfCount = 0;
}

MeshPreviewHint& MeshSdfPreviewCatalog::addMesh(const MeshPreviewHint& hint) {
    if (m_meshCount < m_meshes.size()) {
        m_meshes[m_meshCount] = hint; // copy-assign: the slot's cookedMeshPath keeps its capacity
    } else {
        m_meshes.push_back(hint);
    }
    return m_meshes[m_meshCount++];
}

SdfPreviewHint& MeshSdfPreviewCatalog::addSdf(const SdfPreviewHint& hint) {
    if (m_sdfCount < m_sdfs.size()) {
        m_sdfs[m_sdfCount] = hint;
    } else {
        m_sdfs.push_back(hint);
    }
    return m_sdfs[m_sdfCount++];
}

u32 MeshSdfPreviewCatalog::visibleMeshCount() const {
    u32 count = 0;
    for (const MeshPreviewHint& hint : meshes()) {
        if (hint.visible) {
            ++count;
        }
    }
    return count;
}

u32 MeshSdfPreviewCatalog::visibleSdfCount() const {
    u32 count = 0;
    for (const SdfPreviewHint& hint : sdfs()) {
        if (hint.visible) {
            ++count;
        }
    }
    return count;
}

u32 MeshSdfPreviewCatalog::cookedMeshResolvedCount() const {
    u32 count = 0;
    for (const MeshPreviewHint& hint : meshes()) {
        if (hint.cookedMeshResolved) {
            ++count;
        }
    }
    return count;
}

} // namespace fuse::hybrid
