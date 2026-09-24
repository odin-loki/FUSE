# FUSE Relight RL-4.3 tests: the Relight BSDF. Added from ../CMakeLists.txt.
#
#   rl_bsdf_<suite>          (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree): lut, furnace,
#                            reciprocity, chi2, pdf, sss, thinfilm, parity, mapping (see test_bsdf.cpp).
#   rl_bsdf_vk_parity_slang  (gate;vulkan;relight) Lavapipe with validation + synchronization validation: the Slang
#   rl_bsdf_vk_parity_glsl   implementation / the GLSL fallback == the CPU reference (<= 1e-4). Skip (77) without a
#                            Vulkan backend / ICD / layer / compiler. Configured only where the renderer's Vulkan
#                            backend is (FUSE_VULKAN_BACKEND, fuse_rhi).

add_executable(fuse_relight_bsdf_tests test_bsdf.cpp)
target_link_libraries(fuse_relight_bsdf_tests PRIVATE fuse_relight_material)
if(TARGET fuse_ltc)
    # WP-2.2's DFG LUT: cross-check of the albedo table (same Schlick split, independent bake).
    target_link_libraries(fuse_relight_bsdf_tests PRIVATE fuse_ltc)
    target_compile_definitions(fuse_relight_bsdf_tests PRIVATE FUSE_RL_BSDF_HAS_DFG=1)
endif()
_rl_bsdf_cxx23(fuse_relight_bsdf_tests)
foreach(_suite lut furnace reciprocity chi2 pdf sss thinfilm parity mapping)
    add_test(NAME rl_bsdf_${_suite} COMMAND fuse_relight_bsdf_tests ${_suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_bsdf_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 900)
endforeach()

if(NOT (FUSE_VULKAN_BACKEND AND TARGET fuse_rhi))
    return()
endif()
get_filename_component(_rl_bsdf_rp_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../../Renderer/tests" ABSOLUTE)
add_executable(fuse_relight_bsdf_vk_tests test_bsdf_vk.cpp)
target_include_directories(fuse_relight_bsdf_vk_tests PRIVATE "${_rl_bsdf_rp_tests}")  # rp_wp05_vk.hpp (read-only)
target_link_libraries(fuse_relight_bsdf_vk_tests PRIVATE fuse_relight_bsdf fuse_rhi)
if(RL_BSDF_SLANG_SPV)
    target_compile_definitions(fuse_relight_bsdf_vk_tests PRIVATE "FUSE_RL_BSDF_SLANG_SPV=\"${RL_BSDF_SLANG_SPV}\"")
endif()
if(RL_BSDF_GLSL_SPV)
    target_compile_definitions(fuse_relight_bsdf_vk_tests PRIVATE "FUSE_RL_BSDF_GLSL_SPV=\"${RL_BSDF_GLSL_SPV}\"")
endif()
add_dependencies(fuse_relight_bsdf_vk_tests fuse_relight_bsdf_shaders)
_rl_bsdf_cxx23(fuse_relight_bsdf_vk_tests)
set(_rl_bsdf_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
set(_rl_bsdf_vk_tests "")
foreach(_lang slang glsl)
    if(EXISTS "${_rl_bsdf_lock}")
        add_test(NAME rl_bsdf_vk_parity_${_lang}
                 COMMAND "${_rl_bsdf_lock}" "$<TARGET_FILE:fuse_relight_bsdf_vk_tests>" --language ${_lang})
    else()
        add_test(NAME rl_bsdf_vk_parity_${_lang} COMMAND fuse_relight_bsdf_vk_tests --language ${_lang})
    endif()
    list(APPEND _rl_bsdf_vk_tests rl_bsdf_vk_parity_${_lang})
endforeach()
set_tests_properties(${_rl_bsdf_vk_tests} PROPERTIES
    ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    TIMEOUT 900
    LABELS "gate;vulkan;relight")
