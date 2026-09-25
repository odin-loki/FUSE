# FUSE Relight RL-1.6: vertex capture (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.5). Added from
# ../../cmake/relight_vertex_capture.cmake.
#
# Targets
#   fuse_relight_vertex_capture       CPU only, any platform: the SPIR-V pass (SPIRV-Headers only),
#                                     the 48-byte capture layout, the CPU back-transform, the
#                                     vertexshader hash component and the capture ring. Used by the
#                                     capture tap (tap/capture) and by GeometryCaptureConfig::fromOptions.
#   fuse_relight_vertex_capture_hook  the process-wide hand-over the FUSE-DXVK patches RL-1.6-01/-02
#                                     call (dxvk_hook.hpp); linked into relight_dxvk (both DLLs).
#   (PE) dxvk/fuse_vertex_capture_dxvk.cpp is compiled into relight_tap_d3d9, the d3d9 dispatcher:
#        per-draw capture regions, the readback at Present.
# Tests: ../../tests/vertex_capture.

foreach(_dep fuse_relight_hash fuse_relight_tap fuse_relight_options)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "capture/vertex_capture needs ${_dep}")
    endif()
endforeach()

get_filename_component(_rl_vc_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
# SPIRV-Headers as vendored with DXVK (Engine/lib/dxvk/include/spirv, pinned in its VERSION).
set(_rl_vc_spirv_headers "${_rl_vc_repo}/Engine/lib/dxvk/include/spirv/include")

add_library(fuse_relight_vertex_capture STATIC
    src/back_transform.cpp
    src/capture_ring.cpp
    src/draw_capture.cpp
    src/spirv_vertex_capture.cpp
    src/vs_hash.cpp
)
target_include_directories(fuse_relight_vertex_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(fuse_relight_vertex_capture SYSTEM PRIVATE "${_rl_vc_spirv_headers}")
target_link_libraries(fuse_relight_vertex_capture PUBLIC fuse_relight_hash fuse_relight_tap fuse_relight_options)

add_library(fuse_relight_vertex_capture_hook STATIC src/dxvk_hook.cpp)
target_include_directories(fuse_relight_vertex_capture_hook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# relight_tap.hpp (tap::ShaderModule) only: header-only use of fuse_relight_tap.
target_include_directories(fuse_relight_vertex_capture_hook PRIVATE
    $<TARGET_PROPERTY:fuse_relight_tap,INTERFACE_INCLUDE_DIRECTORIES>)

foreach(_t fuse_relight_vertex_capture fuse_relight_vertex_capture_hook)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()

if(TARGET relight_dxvk)
    # FUSE-DXVK patches RL-1.6-01/-02 (src/dxvk/dxvk_shader_ir.cpp) call dxvk::fuseRelightVertexCapture*.
    # Link only: FUSE's usage requirements stay out of the vendored sources.
    target_link_libraries(relight_dxvk PUBLIC $<LINK_ONLY:fuse_relight_vertex_capture_hook>)
endif()

if(TARGET relight_tap_d3d9 AND TARGET relight_d3d9)
    # The GPU half, inside the d3d9 dispatcher (C++17 with DXVK's private headers, like
    # tap/dxvk/fuse_tap_dxvk.cpp); the dispatcher's hooks are compiled in with this define.
    target_sources(relight_tap_d3d9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dxvk/fuse_vertex_capture_dxvk.cpp)
    target_include_directories(relight_tap_d3d9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dxvk ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(relight_tap_d3d9 PRIVATE FUSE_RELIGHT_HAVE_VERTEX_CAPTURE=1)
    target_link_libraries(relight_d3d9 PRIVATE $<LINK_ONLY:fuse_relight_vertex_capture> $<LINK_ONLY:fuse_relight_vertex_capture_hook>)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/vertex_capture ${CMAKE_CURRENT_BINARY_DIR}/tests)
