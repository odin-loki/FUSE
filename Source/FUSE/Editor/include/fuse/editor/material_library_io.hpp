#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::renderer {
class MaterialSystem;
}

namespace fuse::editor {

struct MaterialLibraryIoResult {
    bool ok = false;
    u32 materialCount = 0;
    std::string error;
};

/// Versioned text material library ("FUSEMATLIB 1") written by the editor on scene save (B6.7).
/// Persists every authoring field of each material in id order: base colour, roughness,
/// metallic, normal strength, emissive colour / intensity, shading model and its parameter block,
/// and procedural function id / seed. Floats are written with round-trip precision, so a
/// save / load cycle reproduces the materials (and their packed GPU rows) bit for bit. Texture
/// handles are runtime bindings and are not persisted (the asset pipeline rebinds them).
MaterialLibraryIoResult saveMaterialLibrary(const renderer::MaterialSystem& materials, const std::string& path);

/// Load a library into a ready `MaterialSystem`: ids already registered are overwritten in place
/// (`updateMaterial`), further ids are registered, so material ids referenced by meshes / SDF
/// objects stay valid. Fails without modifying anything on a malformed file.
MaterialLibraryIoResult loadMaterialLibrary(const std::string& path, renderer::MaterialSystem& materials);

} // namespace fuse::editor
