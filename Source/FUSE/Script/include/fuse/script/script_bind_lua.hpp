#pragma once

#include <fuse/script/script_bind.hpp>

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
extern "C" {
#include <lua.h>
}

namespace fuse::script::bind::lua {

/// Push a tagged `ScriptValue` onto the Lua stack.
void push_to_stack(lua_State* L, const ScriptValue& value);

/// Read a tagged `ScriptValue` from the Lua stack (`index` follows Lua conventions).
[[nodiscard]] ScriptValue read_from_stack(lua_State* L, int index);

} // namespace fuse::script::bind::lua
#endif
