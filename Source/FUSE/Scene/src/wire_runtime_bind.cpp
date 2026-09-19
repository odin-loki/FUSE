#include <fuse/scene/wire_runtime_bind.hpp>

#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/registry.hpp>

namespace fuse::scene {

namespace {

u64 fnv1a64(const std::string& text) {
    u64 hash = 14695981039346656037ull;
    for (unsigned char ch : text) {
        hash ^= static_cast<u64>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

void LegacyDatablockTable::clear() {
    datablocks.clear();
    materials.clear();
}

bool LegacyDatablockTable::findDatablockByOwner(const std::string& owner,
                                                LegacyDatablockEntry* out) const {
    if (out == nullptr) {
        return false;
    }

    for (const LegacyDatablockEntry& entry : datablocks) {
        if (entry.owner == owner) {
            *out = entry;
            return true;
        }
    }
    return false;
}

bool LegacyDatablockTable::findMaterialByOwner(const std::string& owner, LegacyDatablockEntry* out) const {
    if (out == nullptr) {
        return false;
    }

    for (const LegacyDatablockEntry& entry : materials) {
        if (entry.owner == owner) {
            *out = entry;
            return true;
        }
    }
    return false;
}

u32 hashWireRefName(const std::string& refName) {
    return static_cast<u32>(fnv1a64(refName) & 0xFFFFFFFFu);
}

WireRuntimeBindResult populateLegacyTableFromScene(const Scene& scene, LegacyDatablockTable& table) {
    WireRuntimeBindResult result{};
    table.clear();

    for (const SceneEntity& entity : scene.entities()) {
        if (!isWireStubEntityName(entity.name)) {
            continue;
        }

        ++result.wireStubCount;
        const WireStubRef wire = parseWireStubEntityName(entity.name);
        if (!wire.valid) {
            ++result.skipped;
            continue;
        }

        LegacyDatablockEntry entry{};
        entry.owner = wire.owner;
        entry.refName = wire.value;

        if (wire.kind == "datablock") {
            table.datablocks.push_back(std::move(entry));
            ++result.datablockEntries;
        } else if (wire.kind == "material") {
            table.materials.push_back(std::move(entry));
            ++result.materialEntries;
        } else {
            ++result.skipped;
        }
    }

    return result;
}

WireRuntimeBindResult applyWireBindingsToEcs(
    ecs::Registry& registry, const std::unordered_map<std::string, ecs::EntityID>& entitiesByName,
    const LegacyDatablockTable& table) {
    WireRuntimeBindResult result{};
    result.datablockEntries = static_cast<u32>(table.datablocks.size());
    result.materialEntries = static_cast<u32>(table.materials.size());

    for (const LegacyDatablockEntry& material : table.materials) {
        const auto entityIt = entitiesByName.find(material.owner);
        if (entityIt == entitiesByName.end() || !registry.alive(entityIt->second)) {
            ++result.skipped;
            continue;
        }

        const ecs::EntityID entity = entityIt->second;
        if (!registry.has<ecs::Mesh>(entity)) {
            registry.add<ecs::Mesh>(entity);
        }

        ecs::Mesh* mesh = registry.get<ecs::Mesh>(entity);
        if (mesh == nullptr) {
            ++result.skipped;
            continue;
        }

        mesh->material_id = hashWireRefName(material.refName);
        ++result.ecsMaterialApplied;
    }

    for (const LegacyDatablockEntry& datablock : table.datablocks) {
        const auto entityIt = entitiesByName.find(datablock.owner);
        if (entityIt == entitiesByName.end() || !registry.alive(entityIt->second)) {
            ++result.skipped;
            continue;
        }

        ++result.ecsDatablockResolved;
    }

    return result;
}

} // namespace fuse::scene
