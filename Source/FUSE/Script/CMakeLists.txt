# fuse_script — B7.3 Lua-ready script host (null backend + optional Lua)

set(FUSE_SCRIPT_PUBLIC_HEADERS
    include/fuse/script/script_bind.hpp
    include/fuse/script/script_bind_lua.hpp
    include/fuse/script/script_callback.hpp
    include/fuse/script/script_console.hpp
    include/fuse/script/script_host.hpp
    include/fuse/script/script_result.hpp
    include/fuse/script/script_vm.hpp
)

set(FUSE_SCRIPT_SOURCES
    src/script_bind.cpp
    src/script_bind_lua.cpp
    src/script_console.cpp
    src/script_host.cpp
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
if(FUSE_SCRIPT_ENABLE_LUA)
    find_package(Lua QUIET)
    if(Lua_FOUND)
        target_compile_definitions(fuse_script PUBLIC FUSE_SCRIPT_LUA=1)
        target_link_libraries(fuse_script PUBLIC ${LUA_LIBRARIES})
        target_include_directories(fuse_script PUBLIC ${LUA_INCLUDE_DIR})
        message(STATUS "FUSE: fuse_script using system Lua")
    else()
        message(STATUS "FUSE: Lua not found — fuse_script uses null backend")
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
