#pragma once

#include <fuse/ecs/component.hpp>
#include <fuse/types.hpp>

#include <string_view>
#include <type_traits>
#include <typeindex>

namespace fuse::ecs {

/// Stable identity for a component type across runs (type_index is not): its `component_name`.
struct ComponentTypeInfo {
    const char* name = nullptr;
    std::type_index type{typeid(void)};
    usize size = 0;
};

/// Process-wide name <-> type table used by RegistrySerialiser (B3.7). Built-in components are
/// registered by `register_builtin_components()`; other modules register their own types.
class ComponentTypes {
public:
    template <typename T>
    static void register_type() {
        static_assert(IsComponentV<T>, "T must be an ECS component with component_name");
        static_assert(std::is_trivially_copyable_v<T>, "serialised components are copied as bytes");
        register_raw({T::component_name, std::type_index(typeid(T)), sizeof(T)});
    }

    static void register_raw(const ComponentTypeInfo& info);
    [[nodiscard]] static const ComponentTypeInfo* find(std::string_view name);
    [[nodiscard]] static const ComponentTypeInfo* find(std::type_index type);
};

/// Registers Transform, Mesh, RigidBody, SDFObject, Camera, lights, SpawnMarker and tags.
void register_builtin_components();

} // namespace fuse::ecs
