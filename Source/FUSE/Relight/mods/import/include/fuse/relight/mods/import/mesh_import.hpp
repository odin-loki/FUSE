// FUSE Relight RL-3.2: UsdGeomMesh prims and UsdGeomXformable transforms of a composed mod stage, as the importer
// needs them for POCO `Mesh` records (plan §4.4; Remaster §2.3 stream layout).
//
// Mesh (importMesh):
//   * topology: faceVertexCounts / faceVertexIndices, holeIndices skipped, faces fan-triangulated
//     (c0, ci, ci+1); `orientation = "leftHanded"` flips the winding so every output triangle is right-handed;
//     faces with fewer than 3 vertices are skipped with a warning, out-of-range indices reject the mesh;
//   * primvars: normals (`primvars:normals` wins over `normals`), texcoords (`primvars:st`, else the first
//     texCoord2f[] primvar by name), `primvars:displayColor` + `primvars:displayOpacity`, skinning
//     (`primvars:skel:jointIndices` / `jointWeights` with elementSize); interpolation constant / uniform /
//     vertex / varying / faceVarying, indexed primvars (`<name>:indices`). Values are the attribute default, or
//     its earliest time sample (reported);
//   * vertices: when every primvar is per-vertex (vertex / varying / constant) the points keep their order, so
//     a captured mesh comes back unchanged; otherwise (faceVarying or uniform data) every face corner becomes a
//     vertex and identical corners are welded in first-use order;
//   * GeomSubsets (elementType face): one submesh per subset in composed child order, then one for the faces no
//     subset claims; each carries the subset's material binding (else the mesh's);
//   * streams are encoded exactly like RL-1.8's POCO store (Position / Normal F32x3, Uv0 F32x2 in USD `st`
//     convention, Color0 Unorm8x4, Joints0 U16x4 + Weights0 F32x4 when at most 4 influences, indices32).
//
// Transforms (localTransform / relativeTransform): xformOpOrder with translate, scale, rotateX/Y/Z, the six
// rotateABC orders, orient (quaternion), transform, the `!invert!` prefix and `!resetXformStack!`; matrices
// are row-major doubles in the USD / D3D row-vector convention (translation in m[12..14]), as in RL-1.8.
#pragma once

#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fuse::relight::mods::import {

using Mat4d = std::array<double, 16>;
Mat4d identityMatrix();
/// Row-vector product: a then b (v * a * b).
Mat4d multiplyMatrix(const Mat4d& a, const Mat4d& b);

struct TransformIssue {
    std::string message;
};

/// The prim's local transform (identity without xformOpOrder). `resetsStack` is set by `!resetXformStack!`.
Mat4d localTransform(const usd::Prim& prim, bool* resetsStack = nullptr, std::vector<TransformIssue>* issues = nullptr);

/// Transform of `path` into the space of `ancestor`'s parent: local(path) * local(parent) * ... * local(ancestor)
/// (the ancestor's own ops included, as upstream ComputeRelativeTransform(prim, root.GetParent())). An empty
/// `ancestor` means the stage root. Stops at a prim that resets the transform stack.
Mat4d relativeTransform(const usd::ComposedStage& stage, const std::string& path, const std::string& ancestor,
                        std::vector<TransformIssue>* issues = nullptr);

struct ImportedSubmesh {
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    std::string subsetPath;   ///< GeomSubset prim ("" for the mesh's own faces)
    std::string materialPath; ///< bound material prim ("" when none)
};

struct ImportedMesh {
    std::vector<std::array<float, 3>> points;
    std::vector<std::array<float, 3>> normals; ///< empty or one per point
    std::vector<std::array<float, 2>> uv0;     ///< empty or one per point
    std::vector<std::array<float, 4>> colors;  ///< empty or one per point
    std::uint32_t influences = 0;              ///< skinning influences per vertex (0: not skinned)
    std::vector<std::int32_t> jointIndices;    ///< influences per point
    std::vector<float> jointWeights;           ///< influences per point
    std::vector<std::uint32_t> indices;        ///< triangle list
    std::vector<ImportedSubmesh> submeshes;
    std::array<float, 6> bounds{};             ///< min xyz, max xyz of the points
    bool doubleSided = false;
    bool leftHanded = false;   ///< authored orientation (the winding was flipped)
    bool expanded = false;     ///< face-corner vertices (faceVarying / uniform primvars)
    std::string uvPrimvar;     ///< the texcoord primvar used ("" when none)
};

struct MeshIssue {
    bool error = false;
    std::string message;
};

/// Reads one UsdGeomMesh prim (issue messages do not repeat the prim path). `bindingRoot` bounds the ancestor walk for material:binding (the replacement
/// root; "" = up to the stage root). nullopt when the mesh has no usable topology (reason in `issues`).
std::optional<ImportedMesh> importMesh(const usd::ComposedStage& stage, const usd::Prim& mesh, const std::string& bindingRoot,
                                       std::vector<MeshIssue>* issues);

/// material:binding of `path`, else of its ancestors up to and including `root` (composed targets; "" = none).
std::string boundMaterial(const usd::ComposedStage& stage, const std::string& path, const std::string& root);

/// Little-endian stream bytes as RL-1.8's POCO store writes them.
std::vector<std::uint8_t> positionStream(const ImportedMesh& m);
std::vector<std::uint8_t> normalStream(const ImportedMesh& m);
std::vector<std::uint8_t> uv0Stream(const ImportedMesh& m);
std::vector<std::uint8_t> color0Stream(const ImportedMesh& m);
std::vector<std::uint8_t> joints0Stream(const ImportedMesh& m);
std::vector<std::uint8_t> weights0Stream(const ImportedMesh& m);
std::vector<std::uint8_t> indices32Stream(const ImportedMesh& m);

} // namespace fuse::relight::mods::import
