#pragma once

#include <fuse/types.hpp>

#include <span>
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
    bool cookedMeshResolved = false;
    /// Editor material colour (GUI-3 catalog); overrides the id-hashed stub tint when set.
    bool hasColor = false;
    u8 r = 0, g = 0, b = 0;
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
    bool hasColor = false;
    u8 r = 0, g = 0, b = 0;
};

/// Headless-safe mesh/SDF preview catalog — software placeholder draws colored stubs.
///
/// The editor rebuilds the catalog every frame (clear() + add*()). clear() keeps the hint slots and their
/// strings, so a steady frame with the same scene re-fills them without allocating.
class MeshSdfPreviewCatalog {
public:
    void clear();

    /// Returns the stored slot, so a caller can fill a string field in place (no temporary).
    MeshPreviewHint& addMesh(const MeshPreviewHint& hint);
    SdfPreviewHint& addSdf(const SdfPreviewHint& hint);

    [[nodiscard]] u32 meshCount() const { return m_meshCount; }
    [[nodiscard]] u32 sdfCount() const { return m_sdfCount; }
    [[nodiscard]] u32 visibleMeshCount() const;
    [[nodiscard]] u32 visibleSdfCount() const;
    [[nodiscard]] u32 cookedMeshResolvedCount() const;

    [[nodiscard]] std::span<const MeshPreviewHint> meshes() const { return {m_meshes.data(), m_meshCount}; }
    [[nodiscard]] std::span<const SdfPreviewHint> sdfs() const { return {m_sdfs.data(), m_sdfCount}; }

private:
    std::vector<MeshPreviewHint> m_meshes; ///< slots [0, m_meshCount) are live; the rest are kept for reuse
    std::vector<SdfPreviewHint> m_sdfs;
    u32 m_meshCount = 0;
    u32 m_sdfCount = 0;
};

} // namespace fuse::hybrid
