# fuse_script — B7.3 Lua script host (system Lua, bundled Lua 5.2 fallback, or null backend)

set(FUSE_SCRIPT_PUBLIC_HEADERS
    include/fuse/script/legacy_script_route.hpp
    include/fuse/script/script_bind.hpp
    include/fuse/script/script_bind_lua.hpp
    include/fuse/script/script_callback.hpp
    include/fuse/script/script_console.hpp
    include/fuse/script/script_console_command.hpp
    include/fuse/script/script_console_command_registry.hpp
    include/fuse/script/script_console_history.hpp
    include/fuse/script/script_engine_api.hpp
    include/fuse/script/script_host.hpp
    include/fuse/script/script_host_service.hpp
    include/fuse/script/script_hot_reload.hpp
    include/fuse/script/script_result.hpp
    include/fuse/script/script_runtime.hpp
    include/fuse/script/script_update.hpp
    include/fuse/script/script_vm.hpp
)

set(FUSE_SCRIPT_SOURCES
    src/legacy_script_route.cpp
    src/script_bind.cpp
    src/script_bind_lua.cpp
    src/script_console.cpp
    src/script_console_command_registry.cpp
    src/script_console_history.cpp
    src/script_engine_api.cpp
    src/script_host.cpp
    src/script_host_service.cpp
    src/script_hot_reload.cpp
    src/script_runtime.cpp
    src/script_update.cpp
    src/script_vm.cpp
)

add_library(fuse_script STATIC
    ${FUSE_SCRIPT_SOURCES}
    ${FUSE_SCRIPT_PUBLIC_HEADERS}
)

add_library(fuse::script ALIAS fuse_script)

target_include_directories(fuse_script
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_script PUBLIC fuse_core fuse_ecs)
target_compile_features(fuse_script PUBLIC cxx_std_17)

option(FUSE_SCRIPT_ENABLE_LUA "Link fuse_script against Lua when available" ON)
option(FUSE_SCRIPT_BUNDLED_LUA
    "Build the in-tree Lua 5.2 sources when no system Lua >= 5.2 is found" ON)
set(FUSE_SCRIPT_BUNDLED_LUA_DIR
    "${CMAKE_SOURCE_DIR}/Engine/lib/bullet/examples/ThirdPartyLibs/lua-5.2.3/src"
    CACHE PATH "In-tree Lua 5.2 source directory used by FUSE_SCRIPT_BUNDLED_LUA")

set(FUSE_SCRIPT_LUA_BACKEND "null")
if(FUSE_SCRIPT_ENABLE_LUA)
    find_package(Lua QUIET)
    if(Lua_FOUND OR LUA_FOUND)
        if(DEFINED LUA_VERSION_STRING AND LUA_VERSION_STRING VERSION_LESS "5.2")
            message(STATUS "FUSE: system Lua ${LUA_VERSION_STRING} is older than 5.2 — ignored")
        else()
            target_compile_definitions(fuse_script PUBLIC FUSE_SCRIPT_LUA=1)
            target_link_libraries(fuse_script PUBLIC ${LUA_LIBRARIES})
            target_include_directories(fuse_script PUBLIC ${LUA_INCLUDE_DIR})
            set(FUSE_SCRIPT_LUA_BACKEND "system")
            message(STATUS "FUSE: fuse_script using system Lua ${LUA_VERSION_STRING}")
        endif()
    endif()

    if(FUSE_SCRIPT_LUA_BACKEND STREQUAL "null" AND FUSE_SCRIPT_BUNDLED_LUA
       AND EXISTS "${FUSE_SCRIPT_BUNDLED_LUA_DIR}/lapi.c")
        enable_language(C)
        set(_fuse_lua_core
            lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c
            lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c
            lvm.c lzio.c
            lauxlib.c lbaselib.c lbitlib.c lcorolib.c ldblib.c liolib.c lmathlib.c
            loslib.c lstrlib.c ltablib.c loadlib.c linit.c)
        list(TRANSFORM _fuse_lua_core PREPEND "${FUSE_SCRIPT_BUNDLED_LUA_DIR}/")
        add_library(fuse_script_lua STATIC ${_fuse_lua_core})
        set_target_properties(fuse_script_lua PROPERTIES
            LINKER_LANGUAGE C
            POSITION_INDEPENDENT_CODE ON)
        target_include_directories(fuse_script_lua SYSTEM PUBLIC "${FUSE_SCRIPT_BUNDLED_LUA_DIR}")
        if(APPLE)
            target_compile_definitions(fuse_script_lua PRIVATE LUA_USE_MACOSX)
        elseif(UNIX)
            target_compile_definitions(fuse_script_lua PRIVATE LUA_USE_LINUX)
        endif()
        if(MSVC)
            target_compile_options(fuse_script_lua PRIVATE /w)
        else()
            # Lua 5.2 hashes numbers with wrapping signed int arithmetic (ltable.c hashnum):
            # -fwrapv makes that well-defined (UBSan-clean) with identical codegen.
            target_compile_options(fuse_script_lua PRIVATE -w -fwrapv)
        endif()
        if(UNIX)
            target_link_libraries(fuse_script_lua PUBLIC m)
        endif()
        target_compile_definitions(fuse_script PUBLIC FUSE_SCRIPT_LUA=1)
        target_link_libraries(fuse_script PUBLIC fuse_script_lua)
        set(FUSE_SCRIPT_LUA_BACKEND "bundled")
        message(STATUS "FUSE: fuse_script using bundled Lua 5.2 (${FUSE_SCRIPT_BUNDLED_LUA_DIR})")
    endif()

    if(FUSE_SCRIPT_LUA_BACKEND STREQUAL "null")
        message(STATUS "FUSE: Lua not found — fuse_script uses null backend")
    endif()
endif()

# Optional physics bridge: Physics.* script API + collision events on PhysicsManager.
if(TARGET fuse_physics)
    add_library(fuse_script_physics STATIC
        src/script_physics_bridge.cpp
        include/fuse/script/script_physics_bridge.hpp
    )
    add_library(fuse::script_physics ALIAS fuse_script_physics)
    target_link_libraries(fuse_script_physics PUBLIC fuse_script fuse_physics)
    target_compile_features(fuse_script_physics PUBLIC cxx_std_17)
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
