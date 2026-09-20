#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::hybrid {

enum class SdfPreviewPrimitive : u8 {
    Sphere = 0,
    Box = 1,
    Capsule = 2,
    Torus = 3,
    Cylinder = 4,
    Custom = 5,
};

struct MeshPreviewHint {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    std::string cookedMeshPath;
    u32 materialId = 0;
    bool visible = true;
};

struct SdfPreviewHint {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    SdfPreviewPrimitive primitive = SdfPreviewPrimitive::Sphere;
    float param0 = 1.f;
    float param1 = 0.f;
    float param2 = 0.f;
    u32 materialId = 0;
    bool visible = true;
};

/// Headless-safe mesh/SDF preview catalog — software placeholder draws colored stubs.
class MeshSdfPreviewCatalog {
public:
    void clear();

    void addMesh(const MeshPreviewHint& hint);
    void addSdf(const SdfPreviewHint& hint);

    [[nodiscard]] u32 meshCount() const { return static_cast<u32>(m_meshes.size()); }
    [[nodiscard]] u32 sdfCount() const { return static_cast<u32>(m_sdfs.size()); }
    [[nodiscard]] u32 visibleMeshCount() const;
    [[nodiscard]] u32 visibleSdfCount() const;

    [[nodiscard]] const std::vector<MeshPreviewHint>& meshes() const { return m_meshes; }
    [[nodiscard]] const std::vector<SdfPreviewHint>& sdfs() const { return m_sdfs; }

private:
    std::vector<MeshPreviewHint> m_meshes;
    std::vector<SdfPreviewHint> m_sdfs;
};

} // namespace fuse::hybrid
