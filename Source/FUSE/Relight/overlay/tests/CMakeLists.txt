# FUSE Relight RL-6.1 tests. Added from ../CMakeLists.txt.
#
#   rl_overlay_<suite>          (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree), see
#                               test_overlay_cpu.cpp: font, ui, input, script, raster, debug_map, hidden, menu_options,
#                               menu_tags, menu_debug, layout, zero_alloc.
#   rl_pe.overlay_vk            (relight;wine;renderer_pe) the compose pass on Lavapipe (test_overlay_vk.cpp): GPU ==
#   rl_pe.overlay_vk_validation composeRegionCpu for the layer (bit for bit) and the debug views (<= 1 step), Slang and
#                               GLSL, the panel rectangle only, zero steady-state allocations; with the host validation
#                               layer + synchronization validation injected: 0 messages. Native Vulkan trees:
#                               rl_overlay_vk (gate;vulkan;relight).
#   rl_overlay_<case>_<app>     (relight;wine[;golden]) in-process through d3d9.dll (rl_overlay_run.py): hidden =
#                               bit-identical to FUSE_RELIGHT=0 with every scripted game message forwarded and no
#                               overlay round; shown = only the panel rectangle differs from the game's image and it
#                               equals tests/golden/<app>.png; options / tag / capture = scripted edits persisted to
#                               rtx.conf, the tagged hash equals the capture record's, the capture written;
#                               validation = no validation / sync-validation message id or count over the frame tap.

add_executable(fuse_relight_overlay_tests test_overlay_cpu.cpp)
target_link_libraries(fuse_relight_overlay_tests PRIVATE fuse_relight_overlay_core fuse_relight_options)
target_compile_definitions(fuse_relight_overlay_tests PRIVATE "RL_OVERLAY_SHADER_DIR=\"${RL_OVERLAY_SHADER_DIR}\"")
if(TARGET fuse_relight_classify)
    target_link_libraries(fuse_relight_overlay_tests PRIVATE fuse_relight_classify)
endif()
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_overlay_tests)
else()
    set_target_properties(fuse_relight_overlay_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
foreach(_suite font ui input script raster debug_map hidden menu_options menu_tags menu_debug layout zero_alloc)
    add_test(NAME rl_overlay_${_suite} COMMAND fuse_relight_overlay_tests ${_suite}
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_overlay_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 300)
endforeach()
# The menu suites share a scratch directory under the working directory.
set_tests_properties(rl_overlay_menu_options rl_overlay_menu_tags rl_overlay_menu_debug PROPERTIES
    RESOURCE_LOCK fuse_relight_overlay_tmp)

# ---- the compose pass on Lavapipe ---------------------------------------------------------------------------------------
if(TARGET fuse_relight_overlay AND TARGET fuse_rhi)
    add_executable(fuse_relight_overlay_vk_tests test_overlay_vk.cpp)
    target_link_libraries(fuse_relight_overlay_vk_tests PRIVATE fuse_relight_overlay fuse_rhi)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(fuse_relight_overlay_vk_tests)
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
        set(_rl_ov_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
        if(EXISTS "${_rl_ov_lock}")
            add_test(NAME rl_overlay_vk COMMAND "${_rl_ov_lock}" "$<TARGET_FILE:fuse_relight_overlay_vk_tests>")
        else()
            add_test(NAME rl_overlay_vk COMMAND fuse_relight_overlay_vk_tests)
        endif()
        set_tests_properties(rl_overlay_vk PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "gate;vulkan;relight")
    else()
        set(_rl_ov_runner "${FUSE_WINE_XVFB_RUN}")
        if(NOT _rl_ov_runner)
            set(_rl_ov_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
        endif()
        set(_rl_ov_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
        if(NOT _rl_ov_prefix)
            set(_rl_ov_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
        endif()
        add_test(NAME rl_pe.overlay_vk
                 COMMAND "${_rl_ov_runner}" "${_rl_ov_prefix}" "$<TARGET_FILE:fuse_relight_overlay_vk_tests>" --no-validation)
        set_tests_properties(rl_pe.overlay_vk PROPERTIES
            LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 900)
        if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
           EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
            add_test(NAME rl_pe.overlay_vk_validation
                     COMMAND "${_rl_ov_runner}" "${_rl_ov_prefix}" "$<TARGET_FILE:fuse_relight_overlay_vk_tests>"
                             --external-validation)
            set_tests_properties(rl_pe.overlay_vk_validation PROPERTIES
                ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
                LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
                TIMEOUT 900)
        endif()
    endif()
endif()

# ---- in-process (d3d9.dll under Wine) ---------------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT (Python3_Interpreter_FOUND AND TARGET fuse_relight_overlay AND TARGET relight_d3d9 AND TARGET rl_apps AND
        CMAKE_CROSSCOMPILING))
    return()
endif()
get_filename_component(_rl_ov_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_ov_runner "${_rl_ov_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh")
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_ov_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_ov_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
set(_rl_ov_out "${CMAKE_BINARY_DIR}/relight/overlay")
set(_rl_ov_driver "${CMAKE_CURRENT_SOURCE_DIR}/rl_overlay_run.py")
get_filename_component(_rl_ov_taptools "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tap" ABSOLUTE)
function(_rl_overlay_test name case labels)
    set(_exes "")
    foreach(_app IN LISTS ARGN)
        if(NOT TARGET rl_app_${_app})
            return()
        endif()
        list(APPEND _exes --exe "$<TARGET_FILE:rl_app_${_app}>")
    endforeach()
    list(GET ARGN 0 _first)
    list(LENGTH ARGN _n)
    if(_n GREATER 1)
        set(_first ${case})
    endif()
    add_test(NAME ${name}
             COMMAND "${Python3_EXECUTABLE}" "${_rl_ov_driver}" --tap-tools "${_rl_ov_taptools}" ${_exes} --app ${_first}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_ov_runner}" --prefix-root "${_rl_ov_prefix}"
                     ${case} --out "${_rl_ov_out}/${name}")
    set_tests_properties(${name} PROPERTIES LABELS "${labels}" SKIP_RETURN_CODE 77 TIMEOUT 900)
endfunction()
foreach(_app ff_triangle ff_textured ff_lit ff_alpha sky_ui_hud texture_formats d3d8_ff_textured)
    _rl_overlay_test(rl_overlay_hidden_${_app} hidden "relight;golden;wine" ${_app})
    _rl_overlay_test(rl_overlay_shown_${_app} shown "relight;golden;wine" ${_app})
endforeach()
_rl_overlay_test(rl_overlay_options_ff_textured options "relight;wine" ff_textured)
_rl_overlay_test(rl_overlay_tag_ff_textured tag "relight;wine" ff_textured)
_rl_overlay_test(rl_overlay_tag_d3d8_ff_textured tag "relight;wine" d3d8_ff_textured)
_rl_overlay_test(rl_overlay_capture_ff_textured capture "relight;wine" ff_textured)
_rl_overlay_test(rl_overlay_validation validation "relight;wine;vulkan;gate" sky_ui_hud ff_textured)
