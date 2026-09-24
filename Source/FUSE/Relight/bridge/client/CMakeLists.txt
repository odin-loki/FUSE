# FUSE Relight RL-2.2: bridge client proxies (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.4, §2.6, Wave R2).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(bridge/client)
# Ported from dxvk-remix bridge/src/client (MIT, @0867d3c); see the per-file notices for what changed.
#
# Targets
#   fuse_relight_bridge_client_core   portable logic (every tree, Linux included): D3DFORMAT memory
#                                     layout, DirectInput -> window-message translation
#   fuse_relight_bridge_client_obj    the COM proxies, host connection, window/input hooks, DXVK d3d8
#                                     interop (Windows / MinGW trees)
#   relight_bridge_d3d9               bridge client d3d9.dll   -> <build>/relight/bridge/client/
#   relight_bridge_d3d8               DXVK's d3d8 front end (vendored sources, zlib) linked against
#                                     "d3d9.dll": next to the client it runs D3D8 games through the
#                                     bridge                -> <build>/relight/bridge/client/
#   fuse_relight_bridge_stub_host     test-local host (tests/stub_host.cpp): replays the command
#                                     stream on a real DXVK d3d9.dll; RL-2.3 owns the real host
# Tests: tests/CMakeLists.txt (rl_bridge_client_unit, rl_bridge_{x64,x86}_<app>).

add_library(fuse_relight_bridge_client_core STATIC src/format_layout.cpp src/input_translate.cpp)
target_include_directories(fuse_relight_bridge_client_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(fuse_relight_bridge_client_core PUBLIC cxx_std_17)
set_target_properties(fuse_relight_bridge_client_core PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

set(FUSE_RELIGHT_BRIDGE_CLIENT_DIR "${CMAKE_BINARY_DIR}/relight/bridge/client")

if(WIN32 AND MINGW AND TARGET relight_dxvk_util)
    get_filename_component(_rl_bc_dxvk "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Engine/lib/dxvk" ABSOLUTE)

    add_library(fuse_relight_bridge_client_obj OBJECT
        src/connection.cpp
        src/device_state.cpp
        src/objects.cpp
        src/device.cpp
        src/interface.cpp
        src/window.cpp
        src/dxvk_interop.cpp)
    set_target_properties(fuse_relight_bridge_client_obj PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE OFF)
    target_link_libraries(fuse_relight_bridge_client_obj PUBLIC
        fuse_relight_bridge_client_core fuse_relight_bridge_d3d9_executor relight_dxvk_util user32 gdi32)
    # dxvk_interop.cpp includes the vendored DXVK d3d9_bridge.h / util/config (header-only use + config.cpp
    # from relight_dxvk_util).
    target_include_directories(fuse_relight_bridge_client_obj PRIVATE "${_rl_bc_dxvk}/src")
    add_dependencies(fuse_relight_bridge_client_obj fuse_relight_bridge_schema_gen)

    add_library(relight_bridge_d3d9 SHARED src/d3d9.def)
    target_link_libraries(relight_bridge_d3d9 PRIVATE fuse_relight_bridge_client_obj)
    set_target_properties(relight_bridge_d3d9 PROPERTIES
        PREFIX "" OUTPUT_NAME d3d9 IMPORT_PREFIX "lib" IMPORT_SUFFIX ".dll.a"
        RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BRIDGE_CLIENT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/relight/bridge/lib")
    target_link_options(relight_bridge_d3d9 PRIVATE -static -static-libgcc -static-libstdc++
        $<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:-Wl,--enable-stdcall-fixup> $<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:-Wl,--kill-at>)

    # DXVK's d3d8.dll on top of the bridge (vendored sources unchanged; the toolchain's d3d9 import
    # library makes it import D3D9.DLL by name, which resolves to the client next to it).
    _rl_meson_sources("${_rl_bc_dxvk}/src/d3d8/meson.build" d3d8_src _rl_bc_d3d8_src)
    add_library(relight_bridge_d3d8 SHARED ${_rl_bc_d3d8_src} "${_rl_bc_dxvk}/src/d3d8/d3d8.def")
    _rl_cxx17(relight_bridge_d3d8)
    target_include_directories(relight_bridge_d3d8 PRIVATE "${_rl_bc_dxvk}/src/d3d8")
    target_link_libraries(relight_bridge_d3d8 PRIVATE relight_dxvk_util d3d9)
    set_target_properties(relight_bridge_d3d8 PROPERTIES
        PREFIX "" OUTPUT_NAME d3d8 IMPORT_PREFIX "lib" IMPORT_SUFFIX ".dll.a"
        RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BRIDGE_CLIENT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/relight/bridge/lib")
    add_custom_target(relight_bridge_client ALL DEPENDS relight_bridge_d3d9 relight_bridge_d3d8)

    add_executable(fuse_relight_bridge_stub_host tests/stub_host.cpp)
    set_target_properties(fuse_relight_bridge_stub_host PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        OUTPUT_NAME fuse_relight_bridge_stub_host
        RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BRIDGE_CLIENT_DIR}")
    target_link_libraries(fuse_relight_bridge_stub_host PRIVATE
        fuse_relight_bridge_client_core fuse_relight_bridge_ipc relight_dxvk_flags)
    target_include_directories(fuse_relight_bridge_stub_host PRIVATE "${_rl_bc_dxvk}/src")
    target_link_options(fuse_relight_bridge_stub_host PRIVATE -static -static-libgcc -static-libstdc++)
    add_dependencies(fuse_relight_bridge_stub_host fuse_relight_bridge_schema_gen)
elseif(WIN32)
    message(STATUS "FUSE Relight bridge client (RL-2.2): needs the MinGW DXVK build (relight_dxvk_util); client DLLs skipped")
endif()

add_subdirectory(tests)
