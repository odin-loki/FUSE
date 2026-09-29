# B2 CUDA <-> Vulkan interop gates (FUSE_MASTER_PLAN B2 rows "SharedTimeline semaphore correctly serialises
# Vulkan and CUDA execution", "Vulkan-allocated external memory buffer reads back identical data when
# accessed via CUDA pointer", "CUDA surface write to shared texture appears correctly in Vulkan composite
# pass"). One executable (tests/test_cuda_vk_interop_gates.cpp), one ctest per gate:
#   fuse_cuda_vk_buffer_roundtrip / fuse_cuda_vk_timeline_10k / fuse_cuda_vk_surface_composite
#       device gates (RTX 3090): need FUSE_BUILD_CUDA, a CUDA device and a Vulkan device with the same UUID;
#       exit 77 (skip) otherwise. No ICD override (they must run on the NVIDIA driver).
#   fuse_cuda_vk_timeline_10k_cpu_lane
#       the same 10k-frame SharedTimeline protocol and Vulkan shader with the host as the peer lane; runs
#       on Lavapipe in CI (both build trees).
if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

add_executable(fuse_cuda_vk_interop_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_cuda_vk_interop_gates.cpp)
target_link_libraries(fuse_cuda_vk_interop_gates PRIVATE fuse_rhi)
target_compile_definitions(fuse_cuda_vk_interop_gates PRIVATE
    FUSE_SHADER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures")
if(FUSE_CUDA_BACKEND)
    target_sources(fuse_cuda_vk_interop_gates PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../tests/cuda/cuda_vk_interop_gate_kernels.cu)
    target_link_libraries(fuse_cuda_vk_interop_gates PRIVATE CUDA::cudart)
    set_target_properties(fuse_cuda_vk_interop_gates PROPERTIES
        CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_EXTENSIONS OFF)
endif()

set(_fuse_cvk_shader_src "${CMAKE_CURRENT_LIST_DIR}/../tests/shaders/cuda_vk_timeline_lane.comp")
set(_fuse_cvk_shader_out "${CMAKE_CURRENT_BINARY_DIR}/tests/shaders/cuda_vk_timeline_lane.comp.spv")
if(FUSE_GLSLANG_VALIDATOR)
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V "${_fuse_cvk_shader_src}" -o "${_fuse_cvk_shader_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" "${_fuse_cvk_shader_out}")
    endif()
    add_custom_command(OUTPUT "${_fuse_cvk_shader_out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/tests/shaders"
        ${_cmds}
        DEPENDS "${_fuse_cvk_shader_src}"
        COMMENT "glslangValidator cuda_vk_timeline_lane.comp"
        VERBATIM)
    add_custom_target(fuse_cuda_vk_interop_shaders DEPENDS "${_fuse_cvk_shader_out}")
    add_dependencies(fuse_cuda_vk_interop_gates fuse_cuda_vk_interop_shaders)
    target_compile_definitions(fuse_cuda_vk_interop_gates PRIVATE FUSE_CUDA_VK_LANE_SPV="${_fuse_cvk_shader_out}")
endif()

foreach(_gate buffer_roundtrip timeline surface_composite)
    set(_name "fuse_cuda_vk_${_gate}")
    if(_gate STREQUAL "timeline")
        set(_name fuse_cuda_vk_timeline_10k)
    endif()
    add_test(NAME ${_name} COMMAND fuse_cuda_vk_interop_gates ${_gate})
    set_tests_properties(${_name} PROPERTIES
        LABELS "gate;cuda;cuda_device;vulkan" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 600)
endforeach()

# CPU peer lane: Lavapipe in CI (serialised on the ICD lock like the other Lavapipe tests), the system
# driver elsewhere.
set(_fuse_cvk_lvp_icd "/usr/share/vulkan/icd.d/lvp_icd.json")
set(_fuse_cvk_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
if(UNIX AND EXISTS "${_fuse_cvk_lvp_icd}" AND EXISTS "${_fuse_cvk_lock}")
    add_test(NAME fuse_cuda_vk_timeline_10k_cpu_lane
             COMMAND "${_fuse_cvk_lock}" "$<TARGET_FILE:fuse_cuda_vk_interop_gates>" timeline_cpu_lane)
    set_tests_properties(fuse_cuda_vk_timeline_10k_cpu_lane PROPERTIES ENVIRONMENT "VK_ICD_FILENAMES=${_fuse_cvk_lvp_icd}")
else()
    add_test(NAME fuse_cuda_vk_timeline_10k_cpu_lane COMMAND fuse_cuda_vk_interop_gates timeline_cpu_lane)
endif()
set_tests_properties(fuse_cuda_vk_timeline_10k_cpu_lane PROPERTIES
    LABELS "gate;vulkan" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 600)

# Self-test of the race detection: a word corrupted by the peer lane after frame 5000 must be reported by
# the Vulkan lane in frame 5001 (the gate itself fails; this test passes on that exact report).
add_test(NAME fuse_cuda_vk_timeline_cpu_lane_detects_fault COMMAND fuse_cuda_vk_interop_gates timeline_cpu_lane)
set_tests_properties(fuse_cuda_vk_timeline_cpu_lane_detects_fault PROPERTIES
    ENVIRONMENT "FUSE_CUDA_VK_TIMELINE_INJECT_FRAME=5000;FUSE_CUDA_VK_TIMELINE_FRAMES=6000"
    PASS_REGULAR_EXPRESSION "frame 5001: Vulkan lane saw peer frame 5000 \\(want 5000\\), 1 bad words"
    LABELS "gate;vulkan" RUN_SERIAL TRUE TIMEOUT 600)
if(UNIX AND EXISTS "${_fuse_cvk_lvp_icd}")
    set_property(TEST fuse_cuda_vk_timeline_cpu_lane_detects_fault APPEND PROPERTY
        ENVIRONMENT "VK_ICD_FILENAMES=${_fuse_cvk_lvp_icd}")
endif()
