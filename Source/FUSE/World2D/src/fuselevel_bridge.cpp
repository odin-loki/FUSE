#include <fuse/world2d/fuselevel_bridge.hpp>

#include <fuse/log/logger.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_stub.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <memory>
#include <vector>

namespace fuse::world2d {

FuselevelLoadResult populateWorld2DFromFuselevel(World2D& world, const std::string& fuselevelPath) {
    FuselevelLoadResult result{};

    fuse::scene::Scene scene;
    const fuse::scene::SerialiseResult loaded = fuse::scene::SceneSerialiser::load(fuselevelPath, scene);
    if (loaded.status != fuse::scene::SerialiseStatus::Ok) {
        result.note = loaded.error.empty() ? "fuselevel load failed" : loaded.error;
        return result;
    }

    const std::vector<fuse::scene::SceneEntity>& entities = scene.entities();
    std::vector<fuse::SceneObject2D*> built(entities.size(), nullptr);

    for (usize index = 0; index < entities.size(); ++index) {
        const fuse::scene::SceneEntity& entity = entities[index];
        if (fuse::scene::isWireStubEntityName(entity.name)) {
            ++result.wireStubCount;
            const fuse::scene::WireStubRef wire = fuse::scene::parseWireStubEntityName(entity.name);
            if (wire.valid) {
                ++result.wireStubResolved;
                WireStubRuntimeEntry entry{};
                entry.kind = wire.kind;
                entry.owner = wire.owner;
                entry.value = wire.value;
                result.wireStubs.push_back(std::move(entry));
            }
            continue;
        }

        auto object = std::make_unique<fuse::SceneObject2D>(entity.name);
        object->setPosition(entity.transform.positionX, entity.transform.positionY);
        built[index] = object.get();
        world.addSprite(built[index]);
        world.adoptOwnedSprite(std::move(object));
    }

    for (usize index = 0; index < entities.size(); ++index) {
        fuse::SceneObject2D* child = built[index];
        if (child == nullptr) {
            continue;
        }

        const s32 parentIndex = entities[index].parentIndex;
        if (parentIndex < 0 || static_cast<usize>(parentIndex) >= built.size()) {
            continue;
        }

        fuse::SceneObject2D* parent = built[static_cast<usize>(parentIndex)];
        if (parent != nullptr) {
            parent->addChild(child);
        }
    }

    u32 entityCount = 0;
    for (fuse::SceneObject2D* node : built) {
        if (node != nullptr) {
            ++entityCount;
        }
    }

    result.entityCount = entityCount;
    result.ok = true;
    result.note = result.entityCount > 0 ? "fuselevel scene graph populated" : "fuselevel contained no scene nodes";
    fuse::log::info("World2D: loaded %u entities (%u wire stubs, %u parsed) from %s", result.entityCount,
                    result.wireStubCount, result.wireStubResolved, fuselevelPath.c_str());
    return result;
}

} // namespace fuse::world2d
