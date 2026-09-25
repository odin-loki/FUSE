# FUSE Relight RL-5.6 terrain baking tests. CPU (test_terrain_bake.cpp): rl_terrain_<suite> (gate;relight; native, PE through
# the Wine emulator in the MinGW tree): golden (a synthetic terrain - base texture, alpha road decal, MODULATE2X
# lightmap, additive highlight, alpha-tested grass - baked to 96 x 64, RGBA8 content hash == the golden), blend (every
# blend mode vs the closed form on a flat quad), raster (coverage: every texel centre of a split quad exactly once;
# top-down depth test; lookup == texels), cache (unchanged inputs: cached; any change: re-baked), zero_alloc.
add_executable(fuse_relight_terrain_tests test_terrain_bake.cpp)
target_link_libraries(fuse_relight_terrain_tests PRIVATE fuse_relight_terrain)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_terrain_tests)
else()
    set_target_properties(fuse_relight_terrain_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
foreach(_suite golden blend raster cache zero_alloc)
    add_test(NAME rl_terrain_${_suite} COMMAND fuse_relight_terrain_tests ${_suite}
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_terrain_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 300)
endforeach()

# Vulkan gates of TerrainBakerGpu (test_terrain_vk.cpp): parity (GPU bake == CPU bake / golden, both languages),
# zero_alloc; 0 validation + sync-validation messages.
#   rl_terrain_vk_<lang>           (gate;vulkan;relight) native Vulkan trees (Lavapipe).
#   rl_pe.terrain_vk               (relight;wine;renderer_pe) PE under the Xvfb runner (MinGW tree), without / with the
#   rl_pe.terrain_vk_validation    host's validation layer injected (--external-validation, negative control).
if(NOT TARGET fuse_relight_terrain_gpu OR NOT (FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND FUSE_RELIGHT_RENDERER)))
    return()
endif()
add_executable(fuse_relight_terrain_vk_tests test_terrain_vk.cpp)
target_link_libraries(fuse_relight_terrain_vk_tests PRIVATE fuse_relight_terrain_gpu fuse_rhi)
_rl_terrain_cxx23(fuse_relight_terrain_vk_tests)
if(NOT CMAKE_CROSSCOMPILING)
    set(_rl_tb_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    set(_rl_tb_vk_tests "")
    foreach(_lang slang glsl)
        if(EXISTS "${_rl_tb_lock}")
            add_test(NAME rl_terrain_vk_${_lang}
                     COMMAND "${_rl_tb_lock}" "$<TARGET_FILE:fuse_relight_terrain_vk_tests>" --language ${_lang})
        else()
            add_test(NAME rl_terrain_vk_${_lang} COMMAND fuse_relight_terrain_vk_tests --language ${_lang})
        endif()
        list(APPEND _rl_tb_vk_tests rl_terrain_vk_${_lang})
    endforeach()
    set_tests_properties(${_rl_tb_vk_tests} PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 900 LABELS "gate;vulkan;relight")
    return()
endif()
set(_rl_tb_runner "${FUSE_WINE_XVFB_RUN}")
if(NOT _rl_tb_runner)
    set(_rl_tb_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
endif()
set(_rl_tb_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
if(NOT _rl_tb_prefix)
    set(_rl_tb_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
add_test(NAME rl_pe.terrain_vk
         COMMAND "${_rl_tb_runner}" "${_rl_tb_prefix}" "$<TARGET_FILE:fuse_relight_terrain_vk_tests>" --no-validation)
set_tests_properties(rl_pe.terrain_vk PROPERTIES
    LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 900)
if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
   EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
    add_test(NAME rl_pe.terrain_vk_validation
             COMMAND "${_rl_tb_runner}" "${_rl_tb_prefix}" "$<TARGET_FILE:fuse_relight_terrain_vk_tests>"
                     --external-validation)
    set_tests_properties(rl_pe.terrain_vk_validation PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
        TIMEOUT 900)
endif()
