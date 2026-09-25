# FUSE Relight RL-3.6 tests: the particle system. Added from ../CMakeLists.txt.
#
#   rl_particles_<suite>      (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree):
#                             curve, desc, rng, parity (CpuReference == CpuParallel at 0/2/4 workers, bit-exact),
#                             fixtures (Tests/relight/fixtures/particles: golden per-frame counts + analytic bounds),
#                             billboards (vs an independent double-precision reference), zero_alloc, bridge (spawning
#                             from replaced / attached geometry through a ReplacementEngine), options.
#   rl_particles_vk_parity_fif<N>, rl_particles_vk_parity_one_step, rl_particles_vk_zero_alloc
#                             (gate;vulkan;relight) Lavapipe with validation + synchronization validation: the GLSL
#                             twin == the CPU reference (integer state exact, floats <= 1e-4 relative), no heap
#                             allocation in steady state. Skip (77) without a Vulkan backend / ICD / layer.

get_filename_component(_rl_pt_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_pt_fixtures "${_rl_pt_repo}/Tests/relight/fixtures/particles")

add_executable(fuse_relight_particles_tests test_particles.cpp)
target_link_libraries(fuse_relight_particles_tests PRIVATE fuse_relight_particles_mods fuse_relight_mods_import fuse_relight_hash)
_rl_particles_cxx23(fuse_relight_particles_tests)
foreach(_suite curve desc rng parity fixtures billboards zero_alloc bridge options)
    add_test(NAME rl_particles_${_suite} COMMAND fuse_relight_particles_tests ${_suite} --fixtures "${_rl_pt_fixtures}"
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_particles_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 600)
endforeach()

if(NOT RL_PARTICLES_VK)
    return()
endif()
add_executable(fuse_relight_particles_vk_tests test_particles_vk.cpp)
target_link_libraries(fuse_relight_particles_vk_tests PRIVATE fuse_relight_particles_vk)
_rl_particles_cxx23(fuse_relight_particles_vk_tests)
set(_rl_pt_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
set(_rl_pt_vk_tests "")
foreach(_case "parity_fif1;parity;1;" "parity_fif2;parity;2;" "parity_one_step;parity;2;--one-step" "zero_alloc;zero_alloc;2;")
    list(GET _case 0 _name)
    list(GET _case 1 _suite)
    list(GET _case 2 _fif)
    list(GET _case 3 _extra)
    if(EXISTS "${_rl_pt_lock}")
        add_test(NAME rl_particles_vk_${_name}
                 COMMAND "${_rl_pt_lock}" "$<TARGET_FILE:fuse_relight_particles_vk_tests>" ${_suite} --frames-in-flight ${_fif} ${_extra})
    else()
        add_test(NAME rl_particles_vk_${_name} COMMAND fuse_relight_particles_vk_tests ${_suite} --frames-in-flight ${_fif} ${_extra})
    endif()
    list(APPEND _rl_pt_vk_tests rl_particles_vk_${_name})
endforeach()
set_tests_properties(${_rl_pt_vk_tests} PROPERTIES
    ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    TIMEOUT 900
    LABELS "gate;vulkan;relight")
