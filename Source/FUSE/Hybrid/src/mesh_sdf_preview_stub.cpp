#include <fuse/hybrid/mesh_sdf_preview_stub.hpp>

namespace fuse::hybrid {

void MeshSdfPreviewCatalog::clear() {
    m_meshes.clear();
    m_sdfs.clear();
}

void MeshSdfPreviewCatalog::addMesh(const MeshPreviewHint& hint) {
    m_meshes.push_back(hint);
}

void MeshSdfPreviewCatalog::addSdf(const SdfPreviewHint& hint) {
    m_sdfs.push_back(hint);
}

u32 MeshSdfPreviewCatalog::visibleMeshCount() const {
    u32 count = 0;
    for (const MeshPreviewHint& hint : m_meshes) {
        if (hint.visible) {
            ++count;
        }
    }
    return count;
}

u32 MeshSdfPreviewCatalog::visibleSdfCount() const {
    u32 count = 0;
    for (const SdfPreviewHint& hint : m_sdfs) {
        if (hint.visible) {
            ++count;
        }
    }
    return count;
}

} // namespace fuse::hybrid
