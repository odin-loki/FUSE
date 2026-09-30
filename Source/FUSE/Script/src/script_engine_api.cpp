#include <fuse/script/script_engine_api.hpp>

#include <fuse/config/cvar.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/script/script_vm.hpp>

#include "script_lua_compat.hpp"

#include <cmath>
#include <new>
#include <string>
#include <string_view>
#include <vector>

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

/// An entity argument is its encoded id, or a script instance table (`self`), whose `entity`
/// field is used — so behaviours can write `Entity.destroy(self)`.
ecs::EntityID check_entity(lua_State* L, int index) {
    if (lua_istable(L, index)) {
        lua_getfield(L, index, "entity");
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            luaL_argerror(L, index, "entity id or table with an 'entity' field expected");
            return ecs::EntityID::null();
        }
        const ecs::EntityID id = decode_entity_id(static_cast<f64>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
        return id;
    }
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
    usize name_len = 0;
    const char* name = luaL_optlstring(L, 1, "", &name_len);
    const ecs::EntityID id = registry.create();
    if (!id.valid()) {
        lua_pushnil(L);
        return 1;
    }
    registry.add<ecs::Transform>(id);
    if (name_len > 0 && bindings_of(L).scene != nullptr) {
        bindings_of(L).scene->on_entity_created(id, std::string_view(name, name_len));
    }
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

// ---- Input ----------------------------------------------------------------------------------

ScriptInputBackend& require_input(lua_State* L) {
    ScriptEngineBindings& b = bindings_of(L);
    if (b.input == nullptr) {
        luaL_error(L, "Input API: no input backend bound");
    }
    return *b.input;
}

std::string_view check_view(lua_State* L, int index) {
    usize len = 0;
    const char* text = luaL_checklstring(L, index, &len);
    return {text, len};
}

platform::Key check_key(lua_State* L, int index) {
    const platform::Key key = platform::keyFromName(check_view(L, index));
    if (key == platform::Key::COUNT) {
        luaL_argerror(L, index, "unknown key name");
    }
    return key;
}

platform::MouseButton check_mouse_button(lua_State* L, int index) {
    const platform::MouseButton button = platform::mouseButtonFromName(check_view(L, index));
    if (button == platform::MouseButton::COUNT) {
        luaL_argerror(L, index, "unknown mouse button (Left, Right, Middle, X1, X2)");
    }
    return button;
}

std::string_view check_action(lua_State* L, ScriptInputBackend& input, int index) {
    const std::string_view action = check_view(L, index);
    if (!input.has_action(action)) {
        luaL_error(L, "Input: unknown action '%s'", lua_tostring(L, index));
    }
    return action;
}

int input_key_pressed(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.key_pressed(check_key(L, 1)) ? 1 : 0);
    return 1;
}

int input_key_held(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.key_held(check_key(L, 1)) ? 1 : 0);
    return 1;
}

int input_key_released(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.key_released(check_key(L, 1)) ? 1 : 0);
    return 1;
}

int input_mouse_pressed(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.mouse_pressed(check_mouse_button(L, 1)) ? 1 : 0);
    return 1;
}

int input_mouse_held(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.mouse_held(check_mouse_button(L, 1)) ? 1 : 0);
    return 1;
}

int input_mouse_released(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.mouse_released(check_mouse_button(L, 1)) ? 1 : 0);
    return 1;
}

int input_mouse_delta(lua_State* L) {
    f32 dx = 0.f;
    f32 dy = 0.f;
    require_input(L).mouse_delta(dx, dy);
    lua_pushnumber(L, dx);
    lua_pushnumber(L, dy);
    return 2;
}

int input_mouse_position(lua_State* L) {
    f32 x = 0.f;
    f32 y = 0.f;
    require_input(L).mouse_position(x, y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

int input_pressed(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.action_pressed(check_action(L, in, 1)) ? 1 : 0);
    return 1;
}

int input_held(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.action_held(check_action(L, in, 1)) ? 1 : 0);
    return 1;
}

int input_released(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushboolean(L, in.action_released(check_action(L, in, 1)) ? 1 : 0);
    return 1;
}

int input_axis(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    lua_pushnumber(L, in.action_axis(check_action(L, in, 1)));
    return 1;
}

int input_gamepad_connected(lua_State* L) {
    ScriptInputBackend& in = require_input(L);
    const lua_Number pad = luaL_optnumber(L, 1, 0.0);
    lua_pushboolean(L, pad >= 0.0 && pad < 4096.0 && in.gamepad_connected(static_cast<u32>(pad)) ? 1 : 0);
    return 1;
}

// ---- Audio ----------------------------------------------------------------------------------

ScriptAudioBackend& require_audio(lua_State* L) {
    ScriptEngineBindings& b = bindings_of(L);
    if (b.audio == nullptr) {
        luaL_error(L, "Audio API: no audio backend bound");
    }
    return *b.audio;
}

u64 check_clip(lua_State* L, int index) {
    const lua_Number n = luaL_checknumber(L, index);
    if (!(n >= 1.0) || n >= 9007199254740992.0) {
        luaL_argerror(L, index, "audio clip handle expected");
    }
    return static_cast<u64>(n);
}

f32 opt_float(lua_State* L, int index, f32 fallback) {
    return static_cast<f32>(luaL_optnumber(L, index, static_cast<lua_Number>(fallback)));
}

int audio_load(lua_State* L) {
    ScriptAudioBackend& audio = require_audio(L);
    const char* path = luaL_checkstring(L, 1);
    const u64 clip = audio.load(path);
    if (clip == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(clip));
    return 1;
}

int audio_play_at(lua_State* L) {
    ScriptAudioBackend& audio = require_audio(L);
    const u64 clip = check_clip(L, 1);
    const ecs::vec3 position = check_vec3(L, 2);
    const f32 volume = opt_float(L, 3, 1.f);
    const f32 pitch = opt_float(L, 4, 1.f);
    lua_pushboolean(L, audio.play_at(clip, position, volume, pitch) ? 1 : 0);
    return 1;
}

int audio_play_2d(lua_State* L) {
    ScriptAudioBackend& audio = require_audio(L);
    const u64 clip = check_clip(L, 1);
    lua_pushboolean(L, audio.play_2d(clip, opt_float(L, 2, 1.f)) ? 1 : 0);
    return 1;
}

// ---- Scene ----------------------------------------------------------------------------------

ScriptSceneBackend& require_scene(lua_State* L) {
    ScriptEngineBindings& b = bindings_of(L);
    if (b.scene == nullptr) {
        luaL_error(L, "Scene API: no scene backend bound");
    }
    return *b.scene;
}

int scene_find_entity(lua_State* L) {
    ScriptSceneBackend& scene = require_scene(L);
    const ecs::EntityID id = scene.find_entity(check_view(L, 1));
    if (!id.valid()) {
        lua_pushnil(L);
        return 1;
    }
    push_entity(L, id);
    return 1;
}

int scene_query_sphere(lua_State* L) {
    ScriptSceneBackend& scene = require_scene(L);
    const ecs::vec3 center = check_vec3(L, 1);
    const f32 radius = static_cast<f32>(luaL_checknumber(L, 2));
    // Scratch lives outside this frame: a Lua error (e.g. out of memory while building the
    // result table) longjmps past this function, so no local may need a destructor.
    thread_local std::vector<ecs::EntityID> t_hits;
    t_hits.clear();
    (void)scene.query_sphere(center, radius, t_hits);
    lua_createtable(L, static_cast<int>(t_hits.size()), 0);
    for (usize i = 0; i < t_hits.size(); ++i) {
        push_entity(L, t_hits[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int scene_set_active_camera(lua_State* L) {
    ScriptSceneBackend& scene = require_scene(L);
    lua_pushboolean(L, scene.set_active_camera(check_entity(L, 1)) ? 1 : 0);
    return 1;
}

int scene_active_camera(lua_State* L) {
    const ecs::EntityID id = require_scene(L).active_camera();
    if (!id.valid()) {
        lua_pushnil(L);
        return 1;
    }
    push_entity(L, id);
    return 1;
}

// ---- SDF ------------------------------------------------------------------------------------

constexpr const char* kPrimitiveNames[] = {"Sphere", "Box", "Capsule", "Torus", "Cylinder", "Custom"};

ecs::SDFObject* sdf_of(lua_State* L, int index) {
    ecs::Registry& registry = require_registry(L);
    const ecs::EntityID id = check_entity(L, index);
    return registry.alive(id) ? registry.get<ecs::SDFObject>(id) : nullptr;
}

int sdf_set_radius(lua_State* L) {
    ecs::SDFObject* sdf = sdf_of(L, 1);
    const lua_Number r = luaL_checknumber(L, 2);
    if (!(r >= 0.0) || !std::isfinite(r)) {
        luaL_argerror(L, 2, "radius must be a finite number >= 0");
    }
    if (sdf == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const f32 radius = static_cast<f32>(r);
    if (sdf->type == ecs::SDFPrimitive::Box) {
        sdf->params.x = radius; // uniform half extents
        sdf->params.y = radius;
        sdf->params.z = radius;
    } else {
        sdf->params.x = radius; // sphere / capsule / cylinder radius, torus major radius
    }
    lua_pushboolean(L, 1);
    return 1;
}

int sdf_set_alpha(lua_State* L) {
    ecs::SDFObject* sdf = sdf_of(L, 1);
    lua_Number a = luaL_checknumber(L, 2);
    if (!std::isfinite(a)) {
        luaL_argerror(L, 2, "alpha must be finite");
    }
    a = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
    if (sdf == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    sdf->blend_alpha = static_cast<f32>(a);
    lua_pushboolean(L, 1);
    return 1;
}

int sdf_set_primitive(lua_State* L) {
    ecs::SDFObject* sdf = sdf_of(L, 1);
    const std::string_view name = check_view(L, 2);
    int found = -1;
    for (int i = 0; i < static_cast<int>(sizeof(kPrimitiveNames) / sizeof(kPrimitiveNames[0])); ++i) {
        const std::string_view candidate = kPrimitiveNames[i];
        if (candidate.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (usize c = 0; c < name.size() && same; ++c) {
            const char x = name[c] >= 'A' && name[c] <= 'Z' ? static_cast<char>(name[c] - 'A' + 'a') : name[c];
            const char y = candidate[c] >= 'A' && candidate[c] <= 'Z' ? static_cast<char>(candidate[c] - 'A' + 'a')
                                                                      : candidate[c];
            same = x == y;
        }
        if (same) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        luaL_argerror(L, 2, "unknown primitive (Sphere, Box, Capsule, Torus, Cylinder, Custom)");
    }
    if (sdf == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // Keep the object about the same size: the new shape's params derive from the old radius
    // (params.x) and keep a still-meaningful second parameter.
    const ecs::SDFPrimitive type = static_cast<ecs::SDFPrimitive>(found);
    const f32 r = sdf->params.x;
    const f32 second = sdf->params.y > 0.f ? sdf->params.y : r;
    switch (type) {
    case ecs::SDFPrimitive::Box:
        sdf->params = {r, r, r, 0.f};
        break;
    case ecs::SDFPrimitive::Capsule:
    case ecs::SDFPrimitive::Cylinder:
        sdf->params = {r, second, 0.f, 0.f};
        break;
    case ecs::SDFPrimitive::Torus:
        sdf->params = {r, sdf->params.y > 0.f && sdf->params.y < r ? sdf->params.y : r * 0.25f, 0.f, 0.f};
        break;
    case ecs::SDFPrimitive::Sphere:
    case ecs::SDFPrimitive::Custom:
        sdf->params = {r, 0.f, 0.f, 0.f};
        break;
    }
    sdf->type = type;
    lua_pushboolean(L, 1);
    return 1;
}

int sdf_get(lua_State* L) {
    const ecs::SDFObject* sdf = sdf_of(L, 1);
    if (sdf == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    const u32 type = static_cast<u32>(sdf->type);
    lua_createtable(L, 0, 4);
    lua_pushstring(L, type < 6u ? kPrimitiveNames[type] : "Custom");
    lua_setfield(L, -2, "primitive");
    lua_pushnumber(L, sdf->params.x);
    lua_setfield(L, -2, "radius");
    lua_pushnumber(L, sdf->blend_alpha);
    lua_setfield(L, -2, "alpha");
    push_vec3(L, sdf->params);
    lua_setfield(L, -2, "params");
    return 1;
}

// ---- CVar -----------------------------------------------------------------------------------

config::CVarRegistry& require_cvars(lua_State* L) {
    ScriptEngineBindings& b = bindings_of(L);
    if (b.cvars == nullptr) {
        luaL_error(L, "CVar API: no cvar registry bound");
    }
    return *b.cvars;
}

int cvar_get(lua_State* L) {
    config::CVarRegistry& cvars = require_cvars(L);
    const config::CVarEntry* entry = cvars.find(check_view(L, 1));
    if (entry == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    switch (entry->type()) {
    case config::CVarType::Bool:
        lua_pushboolean(L, entry->get_bool() ? 1 : 0);
        return 1;
    case config::CVarType::Int:
        lua_pushnumber(L, static_cast<lua_Number>(entry->get_int()));
        return 1;
    case config::CVarType::Float:
        lua_pushnumber(L, static_cast<lua_Number>(entry->get_float()));
        return 1;
    case config::CVarType::String:
    case config::CVarType::Enum: {
        thread_local std::string t_text;
        t_text = entry->value_text();
        lua_pushlstring(L, t_text.data(), t_text.size());
        return 1;
    }
    }
    lua_pushnil(L);
    return 1;
}

int cvar_set(lua_State* L) {
    config::CVarRegistry& cvars = require_cvars(L);
    const std::string_view name = check_view(L, 1);
    thread_local std::string t_text;
    switch (lua_type(L, 2)) {
    case LUA_TBOOLEAN:
        t_text = lua_toboolean(L, 2) != 0 ? "true" : "false";
        break;
    case LUA_TNUMBER:
    case LUA_TSTRING: {
        usize len = 0;
        const char* text = lua_tolstring(L, 2, &len);
        t_text.assign(text, len);
        break;
    }
    default:
        luaL_argerror(L, 2, "boolean, number or string expected");
        return 0;
    }
    const config::CVarResult result = cvars.set(name, t_text, config::CVarSource::Runtime);
    if (config::succeeded(result)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, config::to_string(result));
    return 2;
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

    static const NamedFn kInput[] = {
        {"key_pressed", input_key_pressed},
        {"key_held", input_key_held},
        {"key_released", input_key_released},
        {"mouse_pressed", input_mouse_pressed},
        {"mouse_held", input_mouse_held},
        {"mouse_released", input_mouse_released},
        {"mouse_delta", input_mouse_delta},
        {"mouse_position", input_mouse_position},
        {"pressed", input_pressed},
        {"held", input_held},
        {"released", input_released},
        {"axis", input_axis},
        {"gamepad_connected", input_gamepad_connected},
    };
    static const NamedFn kAudio[] = {
        {"load", audio_load},
        {"play_at", audio_play_at},
        {"play_2d", audio_play_2d},
    };
    static const NamedFn kScene[] = {
        {"find_entity", scene_find_entity},
        {"query_sphere", scene_query_sphere},
        {"set_active_camera", scene_set_active_camera},
        {"active_camera", scene_active_camera},
    };
    static const NamedFn kSdf[] = {
        {"set_radius", sdf_set_radius},
        {"set_alpha", sdf_set_alpha},
        {"set_primitive", sdf_set_primitive},
        {"get", sdf_get},
    };
    static const NamedFn kCVar[] = {
        {"get", cvar_get},
        {"set", cvar_set},
    };
    register_table(L, bindings_index, "Input", kInput, static_cast<int>(sizeof(kInput) / sizeof(kInput[0])));
    register_table(L, bindings_index, "Audio", kAudio, static_cast<int>(sizeof(kAudio) / sizeof(kAudio[0])));
    register_table(L, bindings_index, "Scene", kScene, static_cast<int>(sizeof(kScene) / sizeof(kScene[0])));
    register_table(L, bindings_index, "SDF", kSdf, static_cast<int>(sizeof(kSdf) / sizeof(kSdf[0])));
    register_table(L, bindings_index, "CVar", kCVar, static_cast<int>(sizeof(kCVar) / sizeof(kCVar[0])));
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
