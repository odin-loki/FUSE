#include <fuse/world2d/fuselevel_bridge.hpp>

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_runtime_bind.hpp>
#include <fuse/scene/wire_stub.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/scene_transform.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <filesystem>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fuse::world2d {

namespace {

/// Physics settings of a 2D level entity (UNI-U7-WORLD-1 encoding, see saveWorld2DToFuselevel).
void applyColliderToSprite(const ecs::Collider& collider, fuse::SceneObject2D& sprite) {
    sprite.setPhysicsEnabled(true);
    const u32 shape2d = collider.shape_piece;
    sprite.setPhysicsShape(shape2d <= static_cast<u32>(fuse::PhysicsShape2D::Box) ? static_cast<fuse::PhysicsShape2D>(shape2d)
                                                                                   : fuse::PhysicsShape2D::None);
    if (collider.shape == ecs::Collider::Box) {
        sprite.setBoxHalfWidth(collider.params.x);
        sprite.setBoxHalfHeight(collider.params.y);
        sprite.setPhysicsRadius(collider.scalar);
    } else {
        sprite.setPhysicsRadius(collider.params.x);
        sprite.setBoxHalfWidth(collider.params.y);
        sprite.setBoxHalfHeight(collider.params.z);
    }
    sprite.setCollisionLayer(static_cast<s32>(collider.layer));
    sprite.setCollisionMask(collider.mask);
}

} // namespace

FuselevelLoadResult populateWorld2DFromFuselevel(World2D& world, const std::string& fuselevelPath) {
    FuselevelLoadResult result{};

    // UNI-U7-WORLD-1: read the whole file (v3 ECS block included; v1/v2 files get Transform-only entities).
    fuse::scene::Scene scene;
    fuse::ecs::Registry registry;
    fuse::scene::SceneFileInfo info;
    const fuse::scene::SerialiseResult loaded =
        fuse::scene::SceneSerialiser::loadWithRegistry(fuselevelPath, scene, registry, &info);
    if (loaded.status != fuse::scene::SerialiseStatus::Ok) {
        result.note = loaded.error.empty() ? "fuselevel load failed" : loaded.error;
        return result;
    }
    result.fileVersion = info.version;
    result.hasEcsBlock = info.hasEcsBlock;

    // ECS entity index i == scene entity i.
    std::unordered_map<u32, ecs::EntityID> byIndex;
    registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform&) { byIndex.emplace(id.index, id); });

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
        const auto found = byIndex.find(static_cast<u32>(index));
        if (info.hasEcsBlock && found != byIndex.end()) {
            const ecs::Transform& t = *registry.get<ecs::Transform>(found->second);
            object->setLocalTranslation({t.position.x, t.position.y, t.position.z});
            object->setLocalRotation(math::Quat{t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w});
            object->setLocalScale({t.scale.x, t.scale.y, t.scale.z});
            const ecs::Collider* collider = registry.get<ecs::Collider>(found->second);
            if (collider != nullptr && registry.has<ecs::RigidBody>(found->second)) {
                applyColliderToSprite(*collider, *object);
                ++result.physicsBodies;
            }
        } else {
            object->setPosition(entity.transform.positionX, entity.transform.positionY);
        }
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

    // Physics bodies from the level's components (rebuilt after the hierarchy so nested sprites start at
    // their world pose).
    if (result.physicsBodies > 0u) {
        if (world.isPhysicsEnabled()) {
            world.setPhysicsEnabled(false);
        }
        world.setPhysicsEnabled(true);
    }

    result.entityCount = entityCount;
    result.wireBindings = fuse::scene::populateLegacyTableFromScene(scene, result.legacyTable);
    result.ok = true;
    result.note = result.entityCount > 0 ? "fuselevel scene graph populated" : "fuselevel contained no scene nodes";
    fuse::log::info("World2D: loaded %u entities (%u wire stubs, %u parsed, %u physics) from %s (v%u)", result.entityCount,
                    result.wireStubCount, result.wireStubResolved, result.physicsBodies, fuselevelPath.c_str(),
                    result.fileVersion);
    return result;
}

FuselevelSaveResult saveWorld2DToFuselevel(const World2D& world, const std::string& fuselevelPath) {
    FuselevelSaveResult result;
    const SceneObject2D* root = world.root();
    fuse::scene::Scene scene("World2D");
    ecs::Registry registry;
    registry.init();
    std::vector<ecs::EntityID> ids;

    // Pre-order walk of the sprite tree (nodes that are not scene nodes are transparent).
    struct Frame {
        const Object* node;
        s32 parentIndex;
    };
    std::vector<Frame> stack;
    if (root != nullptr) {
        for (auto it = root->children().rbegin(); it != root->children().rend(); ++it) {
            stack.push_back(Frame{*it, -1});
        }
    }
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        const SceneObject2D* sprite = asSceneObject2D(frame.node);
        s32 childParent = frame.parentIndex;
        if (sprite != nullptr) {
            const math::Vec3& p = sprite->localTranslation();
            const math::Quat& q = sprite->localRotation();
            const math::Vec3& s = sprite->localScale();
            fuse::scene::SceneEntityTransform transform{};
            transform.positionX = p.x;
            transform.positionY = p.y;
            transform.positionZ = p.z;
            transform.rotationX = q.x;
            transform.rotationY = q.y;
            transform.rotationZ = q.z;
            transform.rotationW = q.w;
            transform.scaleX = s.x;
            transform.scaleY = s.y;
            transform.scaleZ = s.z;
            scene.addEntity(sprite->name(), transform, frame.parentIndex);
            const ecs::EntityID e = registry.create();
            ids.push_back(e);
            ecs::Transform t{};
            t.position = ecs::vec3{p.x, p.y, p.z, 1.f};
            t.rotation = ecs::quat{q.x, q.y, q.z, q.w};
            t.scale = ecs::vec3{s.x, s.y, s.z, 0.f};
            if (frame.parentIndex >= 0) {
                t.parent = ids[static_cast<usize>(frame.parentIndex)];
            }
            registry.add<ecs::Transform>(e, t);
            if (sprite->physicsEnabled()) {
                ecs::Collider collider{};
                collider.shape_piece = static_cast<u32>(sprite->physicsShape());
                if (sprite->physicsShape() == fuse::PhysicsShape2D::Box) {
                    collider.shape = ecs::Collider::Box;
                    collider.params = ecs::vec3{sprite->boxHalfWidth(), sprite->boxHalfHeight(), sprite->boxHalfWidth(), 0.f};
                    collider.scalar = sprite->physicsRadius();
                } else {
                    collider.shape = ecs::Collider::Sphere;
                    collider.params =
                        ecs::vec3{sprite->physicsRadius(), sprite->boxHalfWidth(), sprite->boxHalfHeight(), 0.f};
                    collider.scalar = 0.f;
                }
                collider.layer = static_cast<u32>(sprite->collisionLayer());
                collider.mask = sprite->collisionMask();
                registry.add<ecs::Collider>(e, collider);
                registry.add<ecs::RigidBody>(e, ecs::RigidBody{});
                ++result.physicsBodies;
            }
            childParent = static_cast<s32>(scene.entityCount() - 1u);
        }
        const std::vector<Object*>& children = frame.node->children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(Frame{*it, childParent});
        }
    }

    // Wire stubs of the last load, under their owners (component-less ECS entities keep index parity).
    const u32 spriteEntities = scene.entityCount();
    for (const WireStubRuntimeEntry& stub : world.lastFuselevelLoad().wireStubs) {
        s32 owner = -1;
        for (u32 i = 0; i < spriteEntities; ++i) {
            if (scene.entities()[i].name == stub.owner) {
                owner = static_cast<s32>(i);
                break;
            }
        }
        scene.addEntity("__fuse.wire|" + stub.kind + "|" + stub.owner + "|" + stub.value, {}, owner);
        ids.push_back(registry.create());
    }

    const std::filesystem::path parent = std::filesystem::path(fuselevelPath).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const fuse::scene::SerialiseResult saved = fuse::scene::SceneSerialiser::saveWithRegistry(
        scene, registry, fuselevelPath, fuse::scene::SceneDimension::World2D);
    if (saved.status != fuse::scene::SerialiseStatus::Ok) {
        result.note = saved.error;
        return result;
    }
    result.ok = true;
    result.entityCount = scene.entityCount();
    result.note = "saved " + std::to_string(result.entityCount) + " entities (" + std::to_string(result.physicsBodies) +
                  " physics) to " + fuselevelPath;
    fuse::log::info("World2D: %s", result.note.c_str());
    return result;
}

} // namespace fuse::world2d
