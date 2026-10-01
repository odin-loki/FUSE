// UNI-U7-WORLD-1: World3D `.fuselevel` load / save (built with FUSE_BUILD_PROJECT, which provides fuse_scene).
//
// The file's v3 ECS block becomes the world's own registry (the one the runtime schedule steps and
// World3D::render draws); the scene table supplies names and the hierarchy for SceneObject3D mirrors.
// Convention (shared with the T3D converter and SceneSerialiser's v1/v2 rebuild): ECS entity index i is
// scene entity i.

#include <fuse/world3d/world_3d.hpp>

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_stub.hpp>
#include <fuse/world2d/scene_transform.hpp>

#include <filesystem>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fuse::world3d {

struct World3D::LevelState {
    scene::Scene scene;
    std::string path;
    /// Mirror node per scene entity (null for wire stubs), in scene order.
    std::vector<std::unique_ptr<SceneObject3D>> nodes;
};

void World3D::LevelStateDeleter::operator()(LevelState* state) const {
    if (state != nullptr) {
        // Children first (reverse creation order: a parent is always created before its children).
        while (!state->nodes.empty()) {
            state->nodes.pop_back();
        }
    }
    delete state;
}

void World3D::clearLevel_() {
    for (usize i = 0; i < m_objects.size();) {
        ObjectEntry& entry = m_objects[i];
        if (!entry.levelOwned) {
            ++i;
            continue;
        }
        if (SceneObject2D* node = m_handles.resolve(entry.handle)) {
            m_handles.unpublish(*node);
        }
        m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(i));
    }
    m_level.reset();
}

namespace {

/// Live entity of every registry slot index that carries a Transform.
std::unordered_map<u32, ecs::EntityID> transformEntitiesByIndex(ecs::Registry& registry) {
    std::unordered_map<u32, ecs::EntityID> out;
    registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform&) { out.emplace(id.index, id); });
    return out;
}

} // namespace

World3DLevelLoadResult World3D::loadWorldFromFuselevel(const std::string& fuselevelPath) {
    World3DLevelLoadResult result;
    result.path = fuselevelPath;

    // Parse into scratch objects first: a failed load leaves the current level untouched.
    auto level = std::unique_ptr<LevelState, LevelStateDeleter>(new LevelState());
    level->path = fuselevelPath;
    ecs::Registry loaded;
    scene::SceneFileInfo info;
    const scene::SerialiseResult status = scene::SceneSerialiser::loadWithRegistry(fuselevelPath, level->scene, loaded, &info);
    if (status.status != scene::SerialiseStatus::Ok) {
        result.note = status.error.empty() ? "fuselevel load failed" : status.error;
        log::warn("World3D: level load failed: %s", result.note.c_str());
        m_lastLevelLoad = result;
        return result;
    }
    if (info.dimension != scene::SceneDimension::World3D) {
        log::warn("World3D: %s is a 2D level; loading its entities into the 3D world", fuselevelPath.c_str());
    }

    // Drop the previous level and the schedule (its PhysicsManager caches entity -> body). Objects added with
    // addObject stay in the world; their bodies move to the new registry below.
    for (ObjectEntry& entry : m_objects) {
        if (!entry.levelOwned) {
            destroyBody_(entry);
        }
    }
    clearLevel_();
    m_schedule.shutdown();
    if (m_registryReady) {
        m_registry.destroy();
    }
    m_registry = std::move(loaded);
    m_registryReady = true;
    ecs::TransformSystemOptions transformOptions;
    transformOptions.parallelDirtyRoots = false;
    ecs::TransformSystem::update(m_registry, transformOptions); // world matrices valid before the first frame

    result.fileVersion = info.version;
    result.hasEcsBlock = info.hasEcsBlock;
    result.sceneEntities = level->scene.entityCount();

    if (m_physicsEnabled) {
        for (ObjectEntry& entry : m_objects) {
            if (SceneObject3D* object = resolve(entry.handle)) {
                createBody_(entry, *object);
            }
        }
    }

    const std::unordered_map<u32, ecs::EntityID> byIndex = transformEntitiesByIndex(m_registry);
    const std::vector<scene::SceneEntity>& entities = level->scene.entities();
    level->nodes.resize(entities.size());
    for (usize i = 0; i < entities.size(); ++i) {
        const scene::SceneEntity& entity = entities[i];
        if (scene::isWireStubEntityName(entity.name)) {
            ++result.wireStubs;
            continue;
        }
        auto node = std::make_unique<SceneObject3D>(entity.name);
        const auto found = byIndex.find(static_cast<u32>(i));
        if (found != byIndex.end()) {
            const ecs::Transform& t = *m_registry.get<ecs::Transform>(found->second);
            node->setLocalTranslation({t.position.x, t.position.y, t.position.z});
            node->setLocalRotation(math::Quat{t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w});
            node->setLocalScale({t.scale.x, t.scale.y, t.scale.z});
        } else {
            const scene::SceneEntityTransform& t = entity.transform;
            node->setLocalTranslation({t.positionX, t.positionY, t.positionZ});
            node->setLocalRotation(math::Quat{t.rotationX, t.rotationY, t.rotationZ, t.rotationW});
            node->setLocalScale({t.scaleX, t.scaleY, t.scaleZ});
        }
        level->nodes[i] = std::move(node);
    }
    // Hierarchy (parents precede children in converter output, but any order works here).
    for (usize i = 0; i < entities.size(); ++i) {
        SceneObject3D* node = level->nodes[i].get();
        if (node == nullptr) {
            continue;
        }
        const s32 parent = entities[i].parentIndex;
        SceneObject3D* parentNode = (parent >= 0 && static_cast<usize>(parent) < level->nodes.size() &&
                                     static_cast<usize>(parent) != i)
                                        ? level->nodes[static_cast<usize>(parent)].get()
                                        : nullptr;
        if (parentNode != nullptr) {
            parentNode->addChild(node);
        } else if (m_root) {
            m_root->addChild(node);
        }
    }
    // Publish + link physics bodies.
    for (usize i = 0; i < entities.size(); ++i) {
        SceneObject3D* node = level->nodes[i].get();
        if (node == nullptr) {
            continue;
        }
        ObjectEntry entry;
        entry.levelOwned = true;
        entry.handle = m_handles.publish(*node);
        const auto found = byIndex.find(static_cast<u32>(i));
        if (found != byIndex.end() && m_registry.has_all<ecs::RigidBody, ecs::Collider>(found->second)) {
            entry.body = found->second;
            const ecs::Transform& t = *m_registry.get<ecs::Transform>(found->second);
            entry.syncedTranslation = math::Vec3{t.position.x, t.position.y, t.position.z};
            entry.syncedRotation = math::Quat{t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w};
            entry.syncedVersion = node->worldVersion();
            entry.synced = true;
            ++result.physicsLinked;
        }
        m_objects.push_back(entry);
        ++result.objects;
    }

    // Component census + default material rows for the meshes' material ids.
    u32 maxMaterial = 0;
    bool anyMesh = false;
    m_registry.each<ecs::Mesh>([&](ecs::EntityID, ecs::Mesh& mesh) {
        ++result.meshes;
        anyMesh = true;
        maxMaterial = std::max(maxMaterial, mesh.material_id);
    });
    m_registry.each<ecs::DirectionalLight>([&](ecs::EntityID, ecs::DirectionalLight&) { ++result.directionalLights; });
    m_registry.each<ecs::PointLight>([&](ecs::EntityID, ecs::PointLight&) { ++result.pointLights; });
    m_registry.each<ecs::SpotLight>([&](ecs::EntityID, ecs::SpotLight&) { ++result.spotLights; });
    m_registry.each<ecs::Collider>([&](ecs::EntityID, ecs::Collider&) { ++result.colliders; });
    m_registry.each<ecs::RigidBody>([&](ecs::EntityID, ecs::RigidBody&) { ++result.rigidBodies; });
    m_registry.each<ecs::SpawnMarker>([&](ecs::EntityID, ecs::SpawnMarker&) { ++result.spawnMarkers; });
    // Material ids are small per-level slots (converter) — cap so a stray id cannot allocate a huge table.
    constexpr u32 kMaxDefaultMaterialRows = 1024u;
    if (anyMesh && maxMaterial < kMaxDefaultMaterialRows) {
        for (u32 id = static_cast<u32>(m_materials.size()); id <= maxMaterial; ++id) {
            setMaterial(id, RenderMaterial3D{});
        }
    }
    result.ecsEntities = static_cast<u32>(m_registry.count());

    m_level = std::move(level);
    result.ok = true;
    result.note = "level loaded (v" + std::to_string(result.fileVersion) + ", " + std::to_string(result.objects) +
                  " objects, " + std::to_string(result.ecsEntities) + " ECS entities, " +
                  std::to_string(result.physicsLinked) + " physics bodies)";
    log::info("World3D: %s: %s", fuselevelPath.c_str(), result.note.c_str());
    m_lastLevelLoad = result;
    return result;
}

bool World3D::saveWorld(const std::string& fuselevelPath, std::string* error) {
    scene::Scene empty(m_level ? m_level->scene.name() : std::string("World3D"));
    scene::Scene& table = m_level ? m_level->scene : empty;
    ecs::Registry& registry = ownRegistry_();

    // Scene-table transforms follow the ECS (bodies moved by physics, scripts, the editor).
    if (m_level) {
        const std::unordered_map<u32, ecs::EntityID> byIndex = transformEntitiesByIndex(registry);
        for (u32 i = 0; i < table.entityCount(); ++i) {
            scene::SceneEntity* entity = table.entityAt(i);
            const auto found = byIndex.find(i);
            if (entity == nullptr || found == byIndex.end() || scene::isWireStubEntityName(entity->name)) {
                continue;
            }
            const ecs::Transform& t = *registry.get<ecs::Transform>(found->second);
            entity->transform.positionX = t.position.x;
            entity->transform.positionY = t.position.y;
            entity->transform.positionZ = t.position.z;
            entity->transform.rotationX = t.rotation.x;
            entity->transform.rotationY = t.rotation.y;
            entity->transform.rotationZ = t.rotation.z;
            entity->transform.rotationW = t.rotation.w;
            entity->transform.scaleX = t.scale.x;
            entity->transform.scaleY = t.scale.y;
            entity->transform.scaleZ = t.scale.z;
        }
    }

    const std::filesystem::path parent = std::filesystem::path(fuselevelPath).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const scene::SerialiseResult saved =
        scene::SceneSerialiser::saveWithRegistry(table, registry, fuselevelPath, scene::SceneDimension::World3D);
    if (saved.status != scene::SerialiseStatus::Ok) {
        if (error != nullptr) {
            *error = saved.error;
        }
        log::warn("World3D: save %s failed: %s", fuselevelPath.c_str(), saved.error.c_str());
        return false;
    }
    log::info("World3D: saved %s (%u scene entities, %u ECS entities)", fuselevelPath.c_str(), table.entityCount(),
              static_cast<u32>(registry.count()));
    return true;
}

bool World3D::findSpawnPoint(f32 outPosition[3]) {
    bool found = false;
    ecs::Registry& registry = ownRegistry_();
    registry.each<ecs::SpawnMarker, ecs::Transform>([&](ecs::EntityID, ecs::SpawnMarker& spawn, ecs::Transform& t) {
        if (found || !spawn.active) {
            return;
        }
        outPosition[0] = t.local_to_world.data[12];
        outPosition[1] = t.local_to_world.data[13];
        outPosition[2] = t.local_to_world.data[14];
        found = true;
    });
    return found;
}

} // namespace fuse::world3d
