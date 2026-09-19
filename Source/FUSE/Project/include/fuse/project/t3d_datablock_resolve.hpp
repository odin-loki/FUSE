#pragma once

#include <fuse/project/importer_extract.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::scene {
class Scene;
}

namespace fuse::project {

struct T3DResolvedBinding {
    std::string owner;
    std::string refName;
    std::string kind; // "datablock" | "material"
    u32 resolvedId = 0;
};

struct T3DDatablockResolveResult {
    std::vector<T3DResolvedBinding> bindings;
    u32 datablockCount = 0;
    u32 materialCount = 0;
    u32 ownerLinked = 0;
};

/// Hash legacy ref names the same way wire-runtime bind does (`hashWireRefName`).
[[nodiscard]] u32 hashT3DLegacyRefName(const std::string& refName);

/// Resolve datablock/material refs from a mission extract onto owning SimObjects.
[[nodiscard]] T3DDatablockResolveResult resolveT3DMissionBindings(const T3DMissionExtract& extract);

/// Resolve wire-stub entities from a loaded `.fuselevel` scene into hashed bindings.
[[nodiscard]] T3DDatablockResolveResult resolveT3DBindingsFromScene(const fuse::scene::Scene& scene);

} // namespace fuse::project
