#pragma once

// Private Lua include + version shims (bundled Lua 5.2 and system Lua 5.3/5.4).

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cmath>

namespace fuse::script::detail {

/// True when the value is a number with an integral value (`lua_isinteger` is 5.3+).
inline bool lua_is_integral(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TNUMBER) {
        return false;
    }
#if LUA_VERSION_NUM >= 503
    if (lua_isinteger(L, index)) {
        return true;
    }
#endif
    const lua_Number n = lua_tonumber(L, index);
    return std::floor(n) == n;
}

} // namespace fuse::script::detail
#endif
