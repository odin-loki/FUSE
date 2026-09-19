#pragma once

#include <fuse/scene/scene.hpp>
#include <fuse/scene/wire_stub.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::ecs {
class Registry;
struct EntityID;
struct Mesh;
} // namespace fuse::ecs

namespace fuse::scene {

struct LegacyDatablockEntry {
    std::string owner;
    std::string refName;
};

/// Runtime table distilled from converter `__fuse.wire|datablock|*` / `material|*` stubs.
struct LegacyDatablockTable {
    std::vector<LegacyDatablockEntry> datablocks;
    std::vector<LegacyDatablockEntry> materials;

    void clear();

    [[nodiscard]] bool findDatablockByOwner(const std::string& owner, LegacyDatablockEntry* out) const;
    [[nodiscard]] bool findMaterialByOwner(const std::string& owner, LegacyDatablockEntry* out) const;
};

struct WireRuntimeBindResult {
    u32 wireStubCount = 0;
    u32 datablockEntries = 0;
    u32 materialEntries = 0;
    u32 ecsMaterialApplied = 0;
    u32 ecsDatablockResolved = 0;
    u32 skipped = 0;
};

[[nodiscard]] u32 hashWireRefName(const std::string& refName);

/// Walk scene entities and populate `table` from `__fuse.wire|*` names.
WireRuntimeBindResult populateLegacyTableFromScene(const Scene& scene, LegacyDatablockTable& table);

/// Apply material/datablock wires to ECS entities keyed by scene object name.
WireRuntimeBindResult applyWireBindingsToEcs(
    ecs::Registry& registry, const std::unordered_map<std::string, ecs::EntityID>& entitiesByName,
    const LegacyDatablockTable& table);

} // namespace fuse::scene
