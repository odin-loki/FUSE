#include <fuse/script/script_bind_lua.hpp>

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA

extern "C" {
#include <lauxlib.h>
}

namespace fuse::script::bind::lua {

namespace {

void push_vec3(lua_State* L, const ecs::vec3& v) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z);
    lua_setfield(L, -2, "z");
    lua_pushnumber(L, v.w);
    lua_setfield(L, -2, "w");
}

void push_quat(lua_State* L, const ecs::quat& q) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, q.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, q.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, q.z);
    lua_setfield(L, -2, "z");
    lua_pushnumber(L, q.w);
    lua_setfield(L, -2, "w");
}

bool read_vec3(lua_State* L, int table_index, ecs::vec3& out) {
    const int abs_index = lua_absindex(L, table_index);
    if (!lua_istable(L, abs_index)) {
        return false;
    }

    lua_getfield(L, abs_index, "x");
    lua_getfield(L, abs_index, "y");
    lua_getfield(L, abs_index, "z");
    lua_getfield(L, abs_index, "w");

    if (!lua_isnumber(L, -4) || !lua_isnumber(L, -3) || !lua_isnumber(L, -2) ||
        !lua_isnumber(L, -1)) {
        lua_pop(L, 4);
        return false;
    }

    out.x = static_cast<ecs::f32>(lua_tonumber(L, -4));
    out.y = static_cast<ecs::f32>(lua_tonumber(L, -3));
    out.z = static_cast<ecs::f32>(lua_tonumber(L, -2));
    out.w = static_cast<ecs::f32>(lua_tonumber(L, -1));
    lua_pop(L, 4);
    return true;
}

bool read_quat(lua_State* L, int table_index, ecs::quat& out) {
    const int abs_index = lua_absindex(L, table_index);
    if (!lua_istable(L, abs_index)) {
        return false;
    }

    lua_getfield(L, abs_index, "x");
    lua_getfield(L, abs_index, "y");
    lua_getfield(L, abs_index, "z");
    lua_getfield(L, abs_index, "w");

    if (!lua_isnumber(L, -4) || !lua_isnumber(L, -3) || !lua_isnumber(L, -2) ||
        !lua_isnumber(L, -1)) {
        lua_pop(L, 4);
        return false;
    }

    out.x = static_cast<ecs::f32>(lua_tonumber(L, -4));
    out.y = static_cast<ecs::f32>(lua_tonumber(L, -3));
    out.z = static_cast<ecs::f32>(lua_tonumber(L, -2));
    out.w = static_cast<ecs::f32>(lua_tonumber(L, -1));
    lua_pop(L, 4);
    return true;
}

bool read_entity_table(lua_State* L, int index, ecs::EntityID& out) {
    const int abs_index = lua_absindex(L, index);
    if (!lua_istable(L, abs_index)) {
        return false;
    }

    lua_getfield(L, abs_index, "index");
    lua_getfield(L, abs_index, "generation");
    if (!lua_isinteger(L, -2) && !lua_isnumber(L, -2)) {
        lua_pop(L, 2);
        return false;
    }
    if (!lua_isinteger(L, -1) && !lua_isnumber(L, -1)) {
        lua_pop(L, 2);
        return false;
    }

    out.index = static_cast<u32>(lua_tointeger(L, -2));
    out.generation = static_cast<u32>(lua_tointeger(L, -1));
    lua_pop(L, 2);
    return true;
}

bool read_transform_table(lua_State* L, int index, ecs::Transform& out) {
    const int abs_index = lua_absindex(L, index);
    if (!lua_istable(L, abs_index)) {
        return false;
    }

    lua_getfield(L, abs_index, "position");
    if (!read_vec3(L, lua_gettop(L), out.position)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pop(L, 1);

    lua_getfield(L, abs_index, "rotation");
    if (!read_quat(L, lua_gettop(L), out.rotation)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pop(L, 1);

    lua_getfield(L, abs_index, "scale");
    if (!read_vec3(L, lua_gettop(L), out.scale)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pop(L, 1);

    lua_getfield(L, abs_index, "dirty");
    if (!lua_isboolean(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    out.dirty = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, abs_index, "parent");
    if (!read_entity_table(L, lua_gettop(L), out.parent)) {
        out.parent = ecs::EntityID::null();
    }
    lua_pop(L, 1);

    return true;
}

} // namespace

void push_to_stack(lua_State* L, const ScriptValue& value) {
    switch (value.kind) {
    case ScriptValueKind::Nil:
        lua_pushnil(L);
        break;
    case ScriptValueKind::Bool:
        lua_pushboolean(L, value.bool_value ? 1 : 0);
        break;
    case ScriptValueKind::Number:
        lua_pushnumber(L, value.number_value);
        break;
    case ScriptValueKind::String:
        lua_pushstring(L, value.string_value.c_str());
        break;
    case ScriptValueKind::EntityId:
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, static_cast<lua_Integer>(value.entity_id.index));
        lua_setfield(L, -2, "index");
        lua_pushinteger(L, static_cast<lua_Integer>(value.entity_id.generation));
        lua_setfield(L, -2, "generation");
        break;
    case ScriptValueKind::Transform: {
        lua_createtable(L, 0, 5);
        push_vec3(L, value.transform.position);
        lua_setfield(L, -2, "position");
        push_quat(L, value.transform.rotation);
        lua_setfield(L, -2, "rotation");
        push_vec3(L, value.transform.scale);
        lua_setfield(L, -2, "scale");
        lua_pushboolean(L, value.transform.dirty ? 1 : 0);
        lua_setfield(L, -2, "dirty");
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, static_cast<lua_Integer>(value.transform.parent.index));
        lua_setfield(L, -2, "index");
        lua_pushinteger(L, static_cast<lua_Integer>(value.transform.parent.generation));
        lua_setfield(L, -2, "generation");
        lua_setfield(L, -2, "parent");
        break;
    }
    default:
        lua_pushnil(L);
        break;
    }
}

ScriptValue read_from_stack(lua_State* L, int index) {
    const int type = lua_type(L, index);
    switch (type) {
    case LUA_TNIL:
        return push_nil();
    case LUA_TBOOLEAN:
        return push_bool(lua_toboolean(L, index) != 0);
    case LUA_TNUMBER:
        return push_number(lua_tonumber(L, index));
    case LUA_TSTRING:
        return push_string(lua_tostring(L, index));
    case LUA_TTABLE: {
        ecs::EntityID entity;
        if (read_entity_table(L, index, entity)) {
            return push_entity_id(entity);
        }

        ecs::Transform transform;
        if (read_transform_table(L, index, transform)) {
            return push_transform(transform);
        }

        return push_nil();
    }
    default:
        return push_nil();
    }
}

} // namespace fuse::script::bind::lua

#endif
