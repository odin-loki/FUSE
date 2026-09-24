# WP-1.2 (docs/unification/RENDERER-EXECUTION.md): meshlet cook.
# Owned by WP-1.2; other packages must not edit this file.
#
#   fuse_meshoptimizer        STATIC  vendored meshoptimizer (Engine/lib/meshoptimizer, pinned in VERSION)
#   fuse_geometry             STATIC  meshlet builder, vertex codec / bounds / cull kernels, FMLT format
#                                     (CPU only: no Vulkan, builds in the stub tree)
#   fuse_geometry_cook        STATIC  FMSH v1 cook hook (links fuse_cook_stubs)
#   fuse_meshlet_cook         EXE     offline CLI (sidecars for .fusemesh files, --verify)
#   fuse_rp_meshlet_{codec,build,format,cook}   ctest gates, labels gate;renderer

set(_fuse_geo_dir "${CMAKE_CURRENT_LIST_DIR}/../geometry")
set(_fuse_meshopt_dir "${CMAKE_SOURCE_DIR}/Engine/lib/meshoptimizer")

file(GLOB _fuse_meshopt_sources CONFIGURE_DEPENDS "${_fuse_meshopt_dir}/src/*.cpp")
add_library(fuse_meshoptimizer STATIC ${_fuse_meshopt_sources})
target_include_directories(fuse_meshoptimizer SYSTEM PUBLIC "${_fuse_meshopt_dir}/src" "${_fuse_meshopt_dir}")
fuse_apply_cxx23(fuse_meshoptimizer)
set_target_properties(fuse_meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(fuse_geometry STATIC
    ${_fuse_geo_dir}/src/meshlet_builder.cpp
    ${_fuse_geo_dir}/src/meshlet_format.cpp
    ${_fuse_geo_dir}/include/fuse/renderer/geometry/meshlet_types.hpp
    ${_fuse_geo_dir}/include/fuse/renderer/geometry/meshlet_format.hpp
    ${_fuse_geo_dir}/include/fuse/renderer/geometry/meshlet_builder.hpp
    ${_fuse_geo_dir}/include/fuse/renderer/geometry/vertex_codec_kernel.hpp
    ${_fuse_geo_dir}/include/fuse/renderer/geometry/meshlet_bounds_kernel.hpp
    ${_fuse_geo_dir}/include/fuse/renderer/geometry/meshlet_cull_kernel.hpp
)
target_include_directories(fuse_geometry PUBLIC ${_fuse_geo_dir}/include)
target_link_libraries(fuse_geometry PUBLIC fuse_core PRIVATE fuse_meshoptimizer)
fuse_apply_cxx23(fuse_geometry)

if(FUSE_BUILD_PROJECT)  # fuse_cook_stubs (Tools/FUSE/Cook) is added after Renderer, under this option
    add_library(fuse_geometry_cook STATIC
        ${_fuse_geo_dir}/cook/meshlet_cook_hook.cpp
        ${_fuse_geo_dir}/include/fuse/renderer/geometry/meshlet_cook_hook.hpp)
    target_link_libraries(fuse_geometry_cook PUBLIC fuse_geometry fuse_cook_stubs)
    fuse_apply_cxx23(fuse_geometry_cook)

    add_executable(fuse_meshlet_cook ${_fuse_geo_dir}/tools/fuse_meshlet_cook.cpp)
    target_link_libraries(fuse_meshlet_cook PRIVATE fuse_geometry_cook)
    fuse_apply_cxx23(fuse_meshlet_cook)

    if(FUSE_BUILD_CORE_TESTS)
        add_executable(fuse_rp_meshlet_cook_tests ${_fuse_geo_dir}/tests/test_rp_meshlet_cook.cpp)
        target_link_libraries(fuse_rp_meshlet_cook_tests PRIVATE fuse_geometry_cook)
        target_compile_definitions(fuse_rp_meshlet_cook_tests PRIVATE FUSE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        fuse_apply_cxx23(fuse_rp_meshlet_cook_tests)
        foreach(_suite codec build format cook)
            add_test(NAME fuse_rp_meshlet_${_suite} COMMAND fuse_rp_meshlet_cook_tests ${_suite})
            set_tests_properties(fuse_rp_meshlet_${_suite} PROPERTIES LABELS "gate;renderer" TIMEOUT 600)
        endforeach()
    endif()
endif()
