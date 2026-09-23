#include <fuse/script/script_engine_api.hpp>

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/script/script_vm.hpp>

#include "script_lua_compat.hpp"

#include <cmath>
#include <new>

namespace fuse::script {

f64 encode_entity_id(ecs::EntityID id) {
    return static_cast<f64>(id.generation) * 4294967296.0 + static_cast<f64>(id.index);
}

ecs::EntityID decode_entity_id(f64 value) {
    if (!(value >= 0.0) || value >= 9007199254740992.0) {
        return ecs::EntityID::null();
    }
    const u64 packed = static_cast<u64>(value);
    ecs::EntityID id;
    id.index = static_cast<u32>(packed & 0xFFFFFFFFull);
    id.generation = static_cast<u32>(packed >> 32u);
    return id;
}

ecs::quat quat_from_euler_degrees(const ecs::vec3& euler_degrees) {
    constexpr f64 kHalfDegToRad = 3.14159265358979323846 / 360.0;
    const f64 hx = euler_degrees.x * kHalfDegToRad;
    const f64 hy = euler_degrees.y * kHalfDegToRad;
    const f64 hz = euler_degrees.z * kHalfDegToRad;
    const f64 sx = std::sin(hx), cx = std::cos(hx);
    const f64 sy = std::sin(hy), cy = std::cos(hy);
    const f64 sz = std::sin(hz), cz = std::cos(hz);

    // q_y * q_x
    const f64 yx_x = cy * sx;
    const f64 yx_y = sy * cx;
    const f64 yx_z = -sy * sx;
    const f64 yx_w = cy * cx;
    // (q_y * q_x) * q_z with q_z = (0, 0, sz, cz)
    ecs::quat q;
    q.x = static_cast<f32>(yx_x * cz + yx_y * sz);
    q.y = static_cast<f32>(yx_y * cz - yx_x * sz);
    q.z = static_cast<f32>(yx_z * cz + yx_w * sz);
    q.w = static_cast<f32>(yx_w * cz - yx_z * sz);
    return q;
}

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
namespace {

ScriptEngineBindings& bindings_of(lua_State* L) {
    return *static_cast<ScriptEngineBindings*>(lua_touserdata(L, lua_upvalueindex(1)));
}

ecs::Registry& require_registry(lua_State* L) {
    ScriptEngineBindings& b = bindings_of(L);
    if (b.registry == nullptr) {
        luaL_error(L, "Entity API: no registry bound");
    }
    return *b.registry;
}

ScriptPhysicsBackend& require_physics(lua_State* L) {
    ScriptEngineBindings& b = bindings_of(L);
    if (b.physics == nullptr) {
        luaL_error(L, "Physics API: no physics backend bound");
    }
    return *b.physics;
}

ecs::EntityID check_entity(lua_State* L, int index) {
    return decode_entity_id(static_cast<f64>(luaL_checknumber(L, index)));
}

void push_entity(lua_State* L, ecs::EntityID id) {
    lua_pushnumber(L, static_cast<lua_Number>(encode_entity_id(id)));
}

f32 field_number(lua_State* L, int table, const char* name, f32 fallback) {
    lua_getfield(L, table, name);
    const f32 value = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return value;
}

ecs::vec3 check_vec3(lua_State* L, int index) {
    const int table = lua_absindex(L, index);
    luaL_checktype(L, table, LUA_TTABLE);
    ecs::vec3 v;
    v.x = field_number(L, table, "x", 0.f);
    v.y = field_number(L, table, "y", 0.f);
    v.z = field_number(L, table, "z", 0.f);
    return v;
}

void push_vec3(lua_State* L, const ecs::vec3& v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z);
    lua_setfield(L, -2, "z");
}

// ---- Entity ---------------------------------------------------------------------------------

int entity_create(lua_State* L) {
    ecs::Registry& registry = require_registry(L);
    luaL_optstring(L, 1, "");
    const ecs::EntityID id = registry.create();
    if (!id.valid()) {
        lua_pushnil(L);
        return 1;
    }
    registry.add<ecs::Transform>(id);
    push_entity(L, id);
    return 1;
}

int entity_destroy(lua_State* L) {
    ecs::Registry& registry = require_registry(L);
    const ecs::EntityID id = check_entity(L, 1);
    const bool alive = registry.alive(id);
    if (alive) {
        registry.destroy_entity(id);
    }
    lua_pushboolean(L, alive ? 1 : 0);
    return 1;
}

int entity_alive(lua_State* L) {
    ecs::Registry& registry = require_registry(L);
    lua_pushboolean(L, registry.alive(check_entity(L, 1)) ? 1 : 0);
    return 1;
}

ecs::Transform* transform_of(lua_State* L, ecs::Registry& registry, ecs::EntityID id) {
    (void)L;
    return registry.alive(id) ? registry.get<ecs::Transform>(id) : nullptr;
}

int entity_get_position(lua_State* L) {
    ecs::Registry& registry = require_registry(L);
    const ecs::Transform* t = transform_of(L, registry, check_entity(L, 1));
    if (t == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    push_vec3(L, t->position);
    return 1;
}

int entity_set_position(lua_State* L) {
    ecs::Registry& registry = require_registry(L);
    const ecs::EntityID id = check_entity(L, 1);
    const ecs::vec3 position = check_vec3(L, 2);
    ecs::Transform* t = transform_of(L, registry, id);
    if (t == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    t->position.x = position.x;
    t->position.y = position.y;
    t->position.z = position.z;
    t->dirty = true;
    lua_pushboolean(L, 1);
    return 1;
}

int entity_get_rotation(lua_State* L) {
    ecs::Registry& registry = require_registry(L);
    const ecs::Transform* t = transform_of(L, registry, check_entity(L, 1));
    if (t == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, t->rotation.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, t->rotation.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, t->rotation.z);
    lua_setfield(L, -2, "z");
    lua_pushnumber(L, t->rotation.w);
    lua_setfield(L, -2, "w");
    return 1;
}

int entity_set_rotation_euler(lua_State* L) {
    ecs::Registry& registry = require_registry(L);
    const ecs::EntityID id = check_entity(L, 1);
    const ecs::vec3 euler = check_vec3(L, 2);
    ecs::Transform* t = transform_of(L, registry, id);
    if (t == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    t->rotation = quat_from_euler_degrees(euler);
    t->dirty = true;
    lua_pushboolean(L, 1);
    return 1;
}

// ---- Physics --------------------------------------------------------------------------------

int physics_ray_cast(lua_State* L) {
    ScriptPhysicsBackend& physics = require_physics(L);
    const ecs::vec3 origin = check_vec3(L, 1);
    const ecs::vec3 direction = check_vec3(L, 2);
    const f32 max_distance = static_cast<f32>(luaL_optnumber(L, 3, 1000.0));
    ScriptRayHit hit;
    if (!physics.ray_cast(origin, direction, max_distance, hit)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    push_entity(L, hit.entity);
    lua_setfield(L, -2, "entity");
    push_vec3(L, hit.point);
    lua_setfield(L, -2, "point");
    push_vec3(L, hit.normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit.distance);
    lua_setfield(L, -2, "distance");
    return 1;
}

int physics_apply_impulse(lua_State* L) {
    ScriptPhysicsBackend& physics = require_physics(L);
    const ecs::EntityID id = check_entity(L, 1);
    lua_pushboolean(L, physics.apply_impulse(id, check_vec3(L, 2)) ? 1 : 0);
    return 1;
}

int physics_set_velocity(lua_State* L) {
    ScriptPhysicsBackend& physics = require_physics(L);
    const ecs::EntityID id = check_entity(L, 1);
    lua_pushboolean(L, physics.set_velocity(id, check_vec3(L, 2)) ? 1 : 0);
    return 1;
}

int physics_get_velocity(lua_State* L) {
    ScriptPhysicsBackend& physics = require_physics(L);
    ecs::vec3 velocity;
    if (!physics.get_velocity(check_entity(L, 1), velocity)) {
        lua_pushnil(L);
        return 1;
    }
    push_vec3(L, velocity);
    return 1;
}

struct NamedFn {
    const char* name;
    lua_CFunction fn;
};

void register_table(lua_State* L, int bindings_index, const char* table, const NamedFn* fns, int count) {
    lua_createtable(L, 0, count);
    for (int i = 0; i < count; ++i) {
        lua_pushvalue(L, bindings_index);
        lua_pushcclosure(L, fns[i].fn, 1);
        lua_setfield(L, -2, fns[i].name);
    }
    lua_setglobal(L, table);
}

void bind_api(lua_State* L, void* user) {
    const auto* source = static_cast<const ScriptEngineBindings*>(user);
    void* storage = lua_newuserdata(L, sizeof(ScriptEngineBindings));
    new (storage) ScriptEngineBindings(*source); // trivially destructible; owned by the Lua GC
    const int bindings_index = lua_gettop(L);

    static const NamedFn kEntity[] = {
        {"create", entity_create},
        {"destroy", entity_destroy},
        {"alive", entity_alive},
        {"get_position", entity_get_position},
        {"set_position", entity_set_position},
        {"get_rotation", entity_get_rotation},
        {"set_rotation_euler", entity_set_rotation_euler},
    };
    static const NamedFn kPhysics[] = {
        {"ray_cast", physics_ray_cast},
        {"apply_impulse", physics_apply_impulse},
        {"set_velocity", physics_set_velocity},
        {"get_velocity", physics_get_velocity},
    };
    register_table(L, bindings_index, "Entity", kEntity, static_cast<int>(sizeof(kEntity) / sizeof(kEntity[0])));
    register_table(L, bindings_index, "Physics", kPhysics,
                   static_cast<int>(sizeof(kPhysics) / sizeof(kPhysics[0])));
}

} // namespace
#endif

bool bind_engine_api(ScriptVM& vm, const ScriptEngineBindings& bindings) {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (!vm.has_lua_backend()) {
        return false;
    }
    ScriptEngineBindings copy = bindings;
    return vm.run_protected(bind_api, &copy).ok();
#else
    (void)vm;
    (void)bindings;
    return false;
#endif
}

} // namespace fuse::script
