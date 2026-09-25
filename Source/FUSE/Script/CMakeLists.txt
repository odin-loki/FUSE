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

option(FUSE_SCRIPT_ENABLE_LUA "Link fuse_script against Lua when available" ON)
option(FUSE_SCRIPT_BUNDLED_LUA
    "Build the vendored Lua 5.2.3 (Engine/lib/lua) when no system Lua >= 5.2 is found" ON)
set(_fuse_lua_dir "${FUSE_VENDOR_DIR}/lua")

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
       AND EXISTS "${_fuse_lua_dir}/src/lapi.c")
        if(NOT TARGET fuse_lua)
            add_subdirectory("${_fuse_lua_dir}" "${CMAKE_BINARY_DIR}/fuse_vendor/lua" EXCLUDE_FROM_ALL)
        endif()
        target_compile_definitions(fuse_script PUBLIC FUSE_SCRIPT_LUA=1)
        target_link_libraries(fuse_script PUBLIC fuse_lua)
        set(FUSE_SCRIPT_LUA_BACKEND "bundled")
        message(STATUS "FUSE: fuse_script using bundled Lua 5.2.3 (${_fuse_lua_dir})")
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
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
