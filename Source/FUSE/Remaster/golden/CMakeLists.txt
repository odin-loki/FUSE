# FUSE Remaster W1.8: golden renders of converted Torque levels (docs/plans/FUSE_REMASTER_PLAN.md §4.8, §7 Wave 1).
# Added from Source/FUSE/Remaster/CMakeLists.txt. Needs the renderer's content-golden harness (fuse_content_golden_lib:
# WP-0.7 HeadlessFrameRunner + vendored NVIDIA FLIP) and the track-B chain (fuse_world_converter, World3D).
#
# Gate remaster_golden_render (labels remaster;golden;vulkan;tier_t0): example_level (Templates/BaseGame
# ExampleLevel.mis) and shapes_level (Tests/fixtures/remaster/golden/shapes_level.mis, five TSStatics cooked from the
# in-tree DTS shapes) re-render bit-identically (FLIP == 0) and match Tests/golden/remaster/<scene>.png (mean FLIP
# <= 0.05); a moved TSStatic is detected. Regenerate the goldens with
#   FUSE_UPDATE_GOLDENS=1 ctest --test-dir <build> -R remaster_golden_render

foreach(_dep fuse_content_golden_lib fuse_world3d fuse_world_converter fuse_project fuse_core)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Remaster: ${_dep} missing; remaster_golden_render not built")
        return()
    endif()
endforeach()

set(_rm_renderer_dir "${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer")
get_directory_property(_rm_stock_shaders DIRECTORY "${_rm_renderer_dir}" DEFINITION FUSE_B5_RHI_SHADER_OUT_DIR)
get_directory_property(_rm_harness_shaders DIRECTORY "${_rm_renderer_dir}" DEFINITION FUSE_RP_HARNESS_SHADER_OUT_DIR)
get_directory_property(_rm_lock DIRECTORY "${_rm_renderer_dir}" DEFINITION _fuse_rp_harness_lock)
get_directory_property(_rm_validation_env DIRECTORY "${_rm_renderer_dir}" DEFINITION _fuse_rp_harness_validation_env)

set(_rm_golden_dir "${FUSE_REMASTER_REPO_ROOT}/Tests/golden/remaster")
set(_rm_artifacts "${CMAKE_BINARY_DIR}/remaster_golden_artifacts")
add_executable(remaster_golden_render test_remaster_golden.cpp)
target_link_libraries(remaster_golden_render PRIVATE fuse_content_golden_lib fuse_world3d fuse_world_converter fuse_project fuse_core)
target_compile_definitions(remaster_golden_render PRIVATE
    FUSE_SOURCE_DIR="${FUSE_REMASTER_REPO_ROOT}"
    FUSE_REMASTER_GOLDEN_DIR="${_rm_golden_dir}"
    FUSE_REMASTER_GOLDEN_ARTIFACT_DIR="${_rm_artifacts}"
    FUSE_RP_STOCK_SHADER_DIR="${_rm_stock_shaders}"
    FUSE_RP_HARNESS_SHADER_DIR="${_rm_harness_shaders}")
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(remaster_golden_render)
endif()
if(MSVC)
    target_compile_options(remaster_golden_render PRIVATE /utf-8)
endif()
foreach(_dep fuse_rp_harness_shaders fuse_b5_rhi_shaders)
    if(TARGET ${_dep})
        add_dependencies(remaster_golden_render ${_dep})
    endif()
endforeach()
add_test(NAME remaster_golden_render
         COMMAND ${_rm_lock} "$<TARGET_FILE:remaster_golden_render>" --golden-dir "${_rm_golden_dir}"
                 --artifact-dir "${_rm_artifacts}" --work "${CMAKE_CURRENT_BINARY_DIR}/work")
set_tests_properties(remaster_golden_render PROPERTIES
    ENVIRONMENT "${_rm_validation_env}"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    TIMEOUT 900
    LABELS "remaster;golden;vulkan;tier_t0")
