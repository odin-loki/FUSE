#pragma once

#include <fuse/scene/wire_runtime_bind.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::world2d {

class World2D;

struct WireStubRuntimeEntry {
    std::string kind;
    std::string owner;
    std::string value;
};

struct FuselevelLoadResult {
    bool ok = false;
    u32 entityCount = 0;
    u32 wireStubCount = 0;
    u32 wireStubResolved = 0;
    fuse::scene::WireRuntimeBindResult wireBindings{};
    fuse::scene::LegacyDatablockTable legacyTable{};
    std::string note;
    std::vector<WireStubRuntimeEntry> wireStubs;
};

/// Loads a converted `.fuselevel` into a World2D scene graph. Skips `__fuse.wire|*` entities
/// for sprite creation but records parsed wiring metadata for runtime follow-up.
FuselevelLoadResult populateWorld2DFromFuselevel(World2D& world, const std::string& fuselevelPath);

} // namespace fuse::world2d
