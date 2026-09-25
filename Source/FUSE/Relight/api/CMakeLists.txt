# FUSE Relight RL-6.2: C API, FUSE-native + Remix API 0.6 compatible (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.10,
# Wave R6). Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(api), after the packages it
# drives (RL-1.7 scene/instances, RL-3.4 replace, RL-4.3 render/material, RL-4.4 render/lights).
#
# Targets
#   fuse_relight_api_core  ApiRuntime (include/fuse/relight/api/api_runtime.hpp): the API's objects and frame on the
#                          scene model, replacement engine, BSDF materials and light set. Any platform.
#   fuse_relight_api       the static library of both C front ends (fuse_relight_api.h, remixapi_compat.h) over the
#                          core, for in-process users and the tests.
#   fuse_relight_api_dll   (PE builds with the vendored DXVK) the same front ends compiled with
#                          FUSE_RELIGHT_API_BUILD_DLL (__declspec(dllexport); the D3D9 interop of the Remix front end)
#                          and linked into relight_d3d9: d3d9.dll exports remixapi_InitializeLibrary and fuse_relight_*
#                          next to its .def exports. No DXVK source is patched.
# Tests: tests/ (rl_api_*), the Wine app Tests/relight/apps/remixapi_c.

foreach(_dep fuse_relight_scene_instances fuse_relight_replace fuse_relight_material fuse_relight_lights
             fuse_relight_capture_geometry)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-6.2 api skipped (no ${_dep})")
        return()
    endif()
endforeach()

macro(_rl_api_cxx23 _t)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endmacro()

add_library(fuse_relight_api_core STATIC src/api_runtime.cpp)
target_include_directories(fuse_relight_api_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_api_core
    PUBLIC fuse_relight_scene_instances fuse_relight_lights fuse_relight_material fuse_relight_mods_import
           fuse_relight_hash fuse_relight_options
    PRIVATE fuse_relight_replace fuse_relight_capture_geometry)
_rl_api_cxx23(fuse_relight_api_core)

set(_rl_api_front src/fuse_relight_api.cpp src/remixapi_compat.cpp)
add_library(fuse_relight_api STATIC ${_rl_api_front})
target_link_libraries(fuse_relight_api PUBLIC fuse_relight_api_core)
_rl_api_cxx23(fuse_relight_api)

if(TARGET relight_d3d9)
    add_library(fuse_relight_api_dll OBJECT ${_rl_api_front})
    target_compile_definitions(fuse_relight_api_dll PRIVATE FUSE_RELIGHT_API_BUILD_DLL=1)
    target_link_libraries(fuse_relight_api_dll PRIVATE fuse_relight_api_core)
    _rl_api_cxx23(fuse_relight_api_dll)
    target_sources(relight_d3d9 PRIVATE $<TARGET_OBJECTS:fuse_relight_api_dll> "${CMAKE_CURRENT_SOURCE_DIR}/src/fuse_relight_api.def")
    # Link only: FUSE's usage requirements (the renderer's Vulkan / volk include directories among them) must stay out
    # of the vendored DXVK sources. CMake 3.28 does not honour $<LINK_ONLY:> in a target's own LINK_LIBRARIES, but it
    # does in INTERFACE_LINK_LIBRARIES (the way a static library's private dependencies propagate), hence the shim.
    add_library(fuse_relight_api_d3d9_link INTERFACE)
    set_property(TARGET fuse_relight_api_d3d9_link PROPERTY INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:fuse_relight_api_core>")
    target_link_libraries(relight_d3d9 PRIVATE fuse_relight_api_d3d9_link)
endif()

add_subdirectory(tests)
