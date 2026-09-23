add_executable(fuse_editor_api_tests
    test_command_queue.cpp
)

target_link_libraries(fuse_editor_api_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_command_queue COMMAND fuse_editor_api_tests)

add_executable(fuse_editor_host_tests
    test_editor_host.cpp
)

target_link_libraries(fuse_editor_host_tests PRIVATE fuse_editor_api fuse_core fuse_scene fuse_project)
if(FUSE_BUILD_VULKAN)
    set(FUSE_VULKAN_ICD_LOCK_SCRIPT "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    set(FUSE_VULKAN_TEST_ENV "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
    add_test(NAME fuse_editor_host COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_editor_host_tests>")
    set_tests_properties(fuse_editor_host PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
        RUN_SERIAL TRUE
    )
else()
    add_test(NAME fuse_editor_host COMMAND fuse_editor_host_tests)
endif()

add_executable(fuse_editor_viewport_present_gate_tests
    test_viewport_present_gate.cpp
)

target_link_libraries(fuse_editor_viewport_present_gate_tests PRIVATE fuse_editor_api fuse_core)
if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_editor_viewport_present_gate_tests PRIVATE fuse_rhi)
    add_test(NAME fuse_editor_viewport_present_gate COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_editor_viewport_present_gate_tests>")
    set_tests_properties(fuse_editor_viewport_present_gate PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
        RUN_SERIAL TRUE
    )
else()
    add_test(NAME fuse_editor_viewport_present_gate COMMAND fuse_editor_viewport_present_gate_tests)
endif()

add_executable(fuse_editor_runtime_embed_tests
    test_runtime_embed_session.cpp
)

target_link_libraries(fuse_editor_runtime_embed_tests PRIVATE fuse_editor_api fuse_core)
if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_editor_runtime_embed_tests PRIVATE fuse_hybrid fuse_rhi)
    add_test(NAME fuse_editor_runtime_embed COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_editor_runtime_embed_tests>")
    # Tests load Samples/unification/* by repo-relative path.
    set_tests_properties(fuse_editor_runtime_embed PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
        RUN_SERIAL TRUE
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
else()
    add_test(NAME fuse_editor_runtime_embed COMMAND fuse_editor_runtime_embed_tests)
endif()

add_executable(fuse_editor_panels_tests
    test_editor_panels.cpp
)

target_link_libraries(fuse_editor_panels_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_panels COMMAND fuse_editor_panels_tests)

add_executable(fuse_editor_command_stack_tests
    test_command_stack.cpp
)

target_link_libraries(fuse_editor_command_stack_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_command_stack COMMAND fuse_editor_command_stack_tests)

add_executable(fuse_editor_hierarchy_model_tests
    test_hierarchy_model.cpp
)

target_link_libraries(fuse_editor_hierarchy_model_tests
    PRIVATE
        fuse_editor_api
        fuse_core
        fuse_world2d
        fuse_world3d
)
if(MINGW)
    target_link_options(fuse_editor_hierarchy_model_tests PRIVATE -Wl,--allow-multiple-definition)
endif()
add_test(NAME fuse_editor_hierarchy_model COMMAND fuse_editor_hierarchy_model_tests)

add_executable(fuse_editor_panels_b69_b612_tests
    test_editor_panels_b69_b612.cpp
)

target_link_libraries(fuse_editor_panels_b69_b612_tests PRIVATE fuse_editor_api fuse_core fuse_scene)
add_test(NAME fuse_editor_panels_b69_b612 COMMAND fuse_editor_panels_b69_b612_tests)

add_executable(fuse_editor_phase6_integration_tests
    test_editor_phase6_integration.cpp
)

target_link_libraries(fuse_editor_phase6_integration_tests
    PRIVATE
        fuse_editor_api
        fuse_core
)
add_test(NAME fuse_editor_phase6_integration COMMAND fuse_editor_phase6_integration_tests)

add_executable(fuse_editor_play_session_tests
    test_play_session.cpp
)

target_link_libraries(fuse_editor_play_session_tests PRIVATE fuse_editor_api fuse_core fuse_scene)
add_test(NAME fuse_editor_play_session COMMAND fuse_editor_play_session_tests)

add_executable(fuse_editor_gizmo_system_tests
    test_gizmo_system.cpp
)

target_link_libraries(fuse_editor_gizmo_system_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_gizmo_system COMMAND fuse_editor_gizmo_system_tests)

add_executable(fuse_editor_p5_slice_tests
    test_p5_vertical_slice.cpp
)

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" FUSE_SOURCE_DIR_CMAKE)
target_link_libraries(fuse_editor_p5_slice_tests PRIVATE fuse_editor_api fuse_core fuse_scene fuse_project)
target_compile_definitions(fuse_editor_p5_slice_tests PRIVATE
    FUSE_SOURCE_DIR="${FUSE_SOURCE_DIR_CMAKE}"
)
if(MINGW)
    target_link_options(fuse_editor_p5_slice_tests PRIVATE -Wl,--allow-multiple-definition)
endif()
if(FUSE_BUILD_VULKAN)
    add_test(NAME fuse_editor_p5_slice COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_editor_p5_slice_tests>")
    set_tests_properties(fuse_editor_p5_slice PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RUN_SERIAL TRUE
    )
else()
    add_test(NAME fuse_editor_p5_slice COMMAND fuse_editor_p5_slice_tests)
    set_tests_properties(fuse_editor_p5_slice PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()

# B6 gate suites (B6.13 checklist rows whose logic is Qt-free).
foreach(_fuse_b6_gate IN ITEMS command hierarchy gizmo panels play_mode)
    add_executable(fuse_editor_b6_${_fuse_b6_gate}_gates test_b6_${_fuse_b6_gate}_gates.cpp)
    target_link_libraries(fuse_editor_b6_${_fuse_b6_gate}_gates PRIVATE fuse_editor_api fuse_core)
    fuse_apply_cxx23(fuse_editor_b6_${_fuse_b6_gate}_gates)
    add_test(NAME fuse_editor_b6_${_fuse_b6_gate}_gates COMMAND fuse_editor_b6_${_fuse_b6_gate}_gates)
endforeach()

# B6.3 / B6.5-B6.7 / B6.10 viewport, inspector, material and profiler rows (B6.13 checklist).
add_executable(fuse_editor_b6_viewport_gates test_b6_viewport_gates.cpp)
target_link_libraries(fuse_editor_b6_viewport_gates PRIVATE fuse_editor_api fuse_core)
fuse_apply_cxx23(fuse_editor_b6_viewport_gates)
add_test(NAME fuse_editor_b6_viewport_gates COMMAND fuse_editor_b6_viewport_gates)
set_tests_properties(fuse_editor_b6_viewport_gates PROPERTIES LABELS "gate")

if(FUSE_BUILD_VULKAN)
    add_executable(fuse_editor_b6_material_profiler_gates test_b6_material_profiler_gates.cpp)
    target_link_libraries(fuse_editor_b6_material_profiler_gates PRIVATE fuse_editor_api fuse_core fuse_rhi)
    fuse_apply_cxx23(fuse_editor_b6_material_profiler_gates)
    add_test(NAME fuse_editor_b6_material_profiler_gates COMMAND fuse_editor_b6_material_profiler_gates)
    set_tests_properties(fuse_editor_b6_material_profiler_gates PROPERTIES LABELS "gate")

    # Resize on a real device under VK_LAYER_KHRONOS_validation (Lavapipe via the ICD lock).
    add_executable(fuse_editor_b6_viewport_resize_vk_gates test_b6_viewport_resize_vk_gates.cpp)
    target_link_libraries(fuse_editor_b6_viewport_resize_vk_gates PRIVATE fuse_editor_api fuse_core fuse_rhi)
    target_compile_definitions(fuse_editor_b6_viewport_resize_vk_gates PRIVATE
        FUSE_SHADER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures")
    fuse_apply_cxx23(fuse_editor_b6_viewport_resize_vk_gates)
    if(FUSE_VULKAN_BACKEND)
        add_test(NAME fuse_editor_b6_viewport_resize_vk_gates
            COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_editor_b6_viewport_resize_vk_gates>")
        set_tests_properties(fuse_editor_b6_viewport_resize_vk_gates PROPERTIES
            ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
            RUN_SERIAL TRUE
            LABELS "gate"
        )
    else()
        add_test(NAME fuse_editor_b6_viewport_resize_vk_gates COMMAND fuse_editor_b6_viewport_resize_vk_gates)
        set_tests_properties(fuse_editor_b6_viewport_resize_vk_gates PROPERTIES LABELS "gate")
    endif()
endif()

if(FUSE_BUILD_PARITY_DEMOS)
    add_executable(fuse_u8_parity_embed_pie_tests
        test_u8_parity_embed_pie.cpp
    )
    target_link_libraries(fuse_u8_parity_embed_pie_tests PRIVATE fuse_editor_api fuse_core fuse_project)
    if(FUSE_BUILD_VULKAN)
        target_link_libraries(fuse_u8_parity_embed_pie_tests PRIVATE fuse_hybrid fuse_rhi)
        add_test(NAME fuse_u8_parity_embed_pie_smoke
            COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_u8_parity_embed_pie_tests>")
        set_tests_properties(fuse_u8_parity_embed_pie_smoke PROPERTIES
            ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RUN_SERIAL TRUE
        )
    else()
        add_test(NAME fuse_u8_parity_embed_pie_smoke COMMAND fuse_u8_parity_embed_pie_tests)
        set_tests_properties(fuse_u8_parity_embed_pie_smoke PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
    endif()
    if(COMMAND fuse_apply_asan)
        fuse_apply_asan(fuse_u8_parity_embed_pie_tests)
    endif()
endif()

if(MINGW)
    get_property(_fuse_editor_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_fuse_editor_test IN LISTS _fuse_editor_test_targets)
        if(_fuse_editor_test MATCHES "^fuse_editor_" OR _fuse_editor_test MATCHES "^fuse_u8_")
            target_link_options(${_fuse_editor_test} PRIVATE -Wl,--allow-multiple-definition)
        endif()
    endforeach()
endif()
