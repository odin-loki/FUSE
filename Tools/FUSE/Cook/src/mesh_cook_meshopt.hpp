#pragma once

// Asset plan W0.2 internals shared by mesh_cook.cpp and mesh_cook_meshopt.cpp: the meshopt vertex /
// index codec encoders and the FMSH v2 section writer (LODs, meshlets, cluster DAG); the decoders and
// section reader live in the runtime asset library (fuse_asset, UNI-U7-ASSET-1). See the layout
// comment on `serialize_cooked_mesh` in mesh_cook.hpp.

#include <fuse/cook/mesh_cook.hpp>

#include <string>
#include <vector>

namespace fuse::cook::detail {

[[nodiscard]] bool meshopt_codec_available();

/// Encode `count` elements of `element_bytes` each (zero-padded to a multiple of 4 for the codec).
void meshopt_encode_vertices(const u8* raw, usize count, u32 element_bytes, std::vector<u8>& out);
void meshopt_encode_indices(const u32* indices, usize count, u32 vertex_count, std::vector<u8>& out);

/// True when the mesh carries any W0.2 section (LODs, meshlets, DAG).
[[nodiscard]] bool has_sections(const CookedMesh& mesh);
/// Append u32 section_count + the sections.
void write_sections(const CookedMesh& mesh, bool codec, std::vector<u8>& out);

} // namespace fuse::cook::detail
