#include <fuse/ecs/component_types.hpp>

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <cstring>
#include <mutex>
#include <vector>

namespace fuse::ecs {

namespace {

std::mutex& tableMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<ComponentTypeInfo>& table() {
    static std::vector<ComponentTypeInfo> entries;
    return entries;
}

} // namespace

void ComponentTypes::register_raw(const ComponentTypeInfo& info) {
    std::lock_guard<std::mutex> lock(tableMutex());
    for (const ComponentTypeInfo& existing : table()) {
        if (existing.type == info.type || std::strcmp(existing.name, info.name) == 0) {
            return; // idempotent
        }
    }
    table().push_back(info);
}

const ComponentTypeInfo* ComponentTypes::find(std::string_view name) {
    std::lock_guard<std::mutex> lock(tableMutex());
    for (const ComponentTypeInfo& entry : table()) {
        if (name == entry.name) {
            return &entry;
        }
    }
    return nullptr;
}

const ComponentTypeInfo* ComponentTypes::find(std::type_index type) {
    std::lock_guard<std::mutex> lock(tableMutex());
    for (const ComponentTypeInfo& entry : table()) {
        if (entry.type == type) {
            return &entry;
        }
    }
    return nullptr;
}

void register_builtin_components() {
    ComponentTypes::register_type<Transform>();
    ComponentTypes::register_type<Mesh>();
    ComponentTypes::register_type<RigidBody>();
    ComponentTypes::register_type<SDFObject>();
    ComponentTypes::register_type<Camera>();
    ComponentTypes::register_type<DirectionalLight>();
    ComponentTypes::register_type<PointLight>();
    ComponentTypes::register_type<SpotLight>();
    ComponentTypes::register_type<SpawnMarker>();
    ComponentTypes::register_type<TagStatic>();
    ComponentTypes::register_type<TagPlayer>();
    ComponentTypes::register_type<TagDestroy>();
    ComponentTypes::register_type<Collider>();
    ComponentTypes::register_type<TagKinematic>();
}

} // namespace fuse::ecs
