add_executable(fuse_scene_serial_tests
    test_main.cpp
    test_camera.cpp
    test_scene_serialiser.cpp
)

target_link_libraries(fuse_scene_serial_tests PRIVATE fuse_scene fuse_project)
add_test(NAME fuse_scene_b37_b39 COMMAND fuse_scene_serial_tests)

add_executable(fuse_scene_svo_tests
    test_svo.cpp
)

target_link_libraries(fuse_scene_svo_tests PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_scene_svo COMMAND fuse_scene_svo_tests)

add_executable(fuse_b3_svo_gates test_b3_svo_gates.cpp)
target_link_libraries(fuse_b3_svo_gates PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_b3_svo_gates COMMAND fuse_b3_svo_gates)

add_executable(fuse_b3_scene_gates test_b3_scene_gates.cpp)
target_link_libraries(fuse_b3_scene_gates PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_b3_scene_gates COMMAND fuse_b3_scene_gates)

add_executable(fuse_scene_manager_tests
    test_scene_manager.cpp
)

target_link_libraries(fuse_scene_manager_tests PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_scene_manager COMMAND fuse_scene_manager_tests)

add_executable(fuse_ecs_phase3_integration_tests
    test_phase3_integration.cpp
)

target_link_libraries(fuse_ecs_phase3_integration_tests PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_ecs_phase3_integration COMMAND fuse_ecs_phase3_integration_tests)

add_executable(fuse_scene_wire_stub_tests
    test_wire_stub.cpp
)

target_link_libraries(fuse_scene_wire_stub_tests PRIVATE fuse_scene)
add_test(NAME fuse_scene_wire_stub COMMAND fuse_scene_wire_stub_tests)

add_executable(fuse_scene_wire_runtime_bind_tests
    test_wire_runtime_bind.cpp
)

target_link_libraries(fuse_scene_wire_runtime_bind_tests PRIVATE fuse_scene fuse_ecs fuse_project fuse_world_converter)
add_test(NAME fuse_scene_wire_runtime_bind COMMAND fuse_scene_wire_runtime_bind_tests)

add_executable(fuse_p4_golden_mis
    test_p4_golden_mis.cpp
)

file(TO_CMAKE_PATH
    "${CMAKE_SOURCE_DIR}/Samples/unification/demo_3d_empty/worlds/example.mis"
    FUSE_GOLDEN_MIS_PATH_CMAKE
)

target_link_libraries(fuse_p4_golden_mis PRIVATE fuse_scene fuse_project fuse_core)
target_compile_definitions(fuse_p4_golden_mis PRIVATE
    FUSE_GOLDEN_MIS_PATH="${FUSE_GOLDEN_MIS_PATH_CMAKE}"
)
add_test(NAME fuse_p4_golden_mis COMMAND fuse_p4_golden_mis)
set_tests_properties(fuse_p4_golden_mis PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(fuse_p7_parity_scenes
    test_p7_parity_scenes.cpp
)

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" FUSE_SOURCE_DIR_CMAKE)
target_link_libraries(fuse_p7_parity_scenes PRIVATE fuse_scene fuse_project fuse_world_converter fuse_core)
target_compile_definitions(fuse_p7_parity_scenes PRIVATE
    FUSE_SOURCE_DIR="${FUSE_SOURCE_DIR_CMAKE}"
)
add_test(NAME fuse_p7_parity_scenes COMMAND fuse_p7_parity_scenes)
set_tests_properties(fuse_p7_parity_scenes PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Timing budgets are meaningless while other tests compete for cores.
set_tests_properties(fuse_b3_scene_gates PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")

# Appendix A: scene file magic is 'FUSE'; pre-rename 'ENGC' files load through the compat path.
add_executable(fuse_scene_magic_gates test_scene_magic_gates.cpp)
target_link_libraries(fuse_scene_magic_gates PRIVATE fuse_scene fuse_core)
target_compile_definitions(fuse_scene_magic_gates PRIVATE FUSE_SOURCE_DIR="${FUSE_SOURCE_DIR_CMAKE}")
add_test(NAME fuse_scene_magic_gates COMMAND fuse_scene_magic_gates)
set_tests_properties(fuse_scene_magic_gates PROPERTIES LABELS "gate")

# B3.9: free camera flies a path; draw counts == brute-force frustum counts every step.
add_executable(fuse_b3_free_camera_gates test_b3_free_camera_gates.cpp)
target_link_libraries(fuse_b3_free_camera_gates PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_b3_free_camera_gates COMMAND fuse_b3_free_camera_gates)
set_tests_properties(fuse_b3_free_camera_gates PROPERTIES LABELS "gate")

# B7.10: save -> close -> reload -> save of a simulated scene (.fuselevel + ECS/physics) is bit-identical.
if(TARGET fuse_physics)
    add_executable(fuse_b7_save_reload_gates test_b7_save_reload_gates.cpp)
    target_link_libraries(fuse_b7_save_reload_gates PRIVATE fuse_scene fuse_physics fuse_ecs fuse_core)
    add_test(NAME fuse_b7_save_reload_gates COMMAND fuse_b7_save_reload_gates)
    set_tests_properties(fuse_b7_save_reload_gates PROPERTIES LABELS "gate")
endif()

# B3 row "SceneData SDF object buffer uploads to GPU and ray marcher renders correct scene":
# SceneData -> SDF object buffer -> device-local upload + byte-exact readback (Lavapipe) -> CPU
# reference ray march vs closed-form intersections. fuse_compute is only built with
# FUSE_BUILD_CUDA, so the CPU reference source is compiled in directly. Exit 77 = no Vulkan device.
if(TARGET fuse_rhi)
    add_executable(fuse_b3_sdf_buffer_ray_march test_b3_sdf_buffer_ray_march_gates.cpp
                   ${CMAKE_SOURCE_DIR}/Source/FUSE/Compute/src/ray_march_cpu.cpp)
    target_include_directories(fuse_b3_sdf_buffer_ray_march PRIVATE ${CMAKE_SOURCE_DIR}/Source/FUSE/Compute/include)
    target_link_libraries(fuse_b3_sdf_buffer_ray_march PRIVATE fuse_scene fuse_ecs fuse_rhi fuse_core)
    add_test(NAME fuse_b3_sdf_buffer_ray_march
             COMMAND "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh"
                     "$<TARGET_FILE:fuse_b3_sdf_buffer_ray_march>")
    set_tests_properties(fuse_b3_sdf_buffer_ray_march PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        LABELS "gate;vulkan")
endif()
