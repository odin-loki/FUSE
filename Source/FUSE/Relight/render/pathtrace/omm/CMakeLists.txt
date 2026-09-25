# FUSE Relight RL-5.6: opacity micromaps (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1, §5.7). Added by one line in
# Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(render/pathtrace/omm) (RL-5.6 owns render/pathtrace/omm/**; the
# RL-5.1 render/pathtrace/CMakeLists.txt is not touched).
#
# Targets
#   fuse_relight_omm   the VK_EXT_opacity_micromap micro-triangle ordering, the conservative micromap build from the
#                      legacy alpha test (VkMicromapBuildInfoEXT inputs), the traversal emulation (spec lookup + any-hit
#                      fallback) and the device capability query (omm_vk.cpp; always "unsupported" in the stub backend).
# Tests: tests/ (rl_omm_*).

if(NOT TARGET fuse_core OR NOT TARGET fuse_rt)
    message(STATUS "FUSE Relight: RL-5.6 render/pathtrace/omm skipped (no fuse_core / fuse_rt)")
    return()
endif()

add_library(fuse_relight_omm STATIC src/omm.cpp src/omm_vk.cpp)
target_include_directories(fuse_relight_omm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_omm PUBLIC fuse_core PRIVATE fuse_rt)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_omm)
else()
    set_target_properties(fuse_relight_omm PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(tests)
