# FUSE Relight RL-5.7 tests: post and upscale wiring. Added from ../CMakeLists.txt.
#
#   rl_post_<suite>          (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree), see
#                            test_post_cpu.cpp: composite (== undemodulated radiance with the denoiser off), mip_bias
#                            (log2(render / output)), options (rtx.conf -> PostConfig), upscaler_switch (every in-tree
#                            backend runs, finite; fsr3 without a device / plugin ids fall back with a reason),
#                            nan_guard, ltm (local tone map Look node), determinism, zero_alloc.
#   rl_post_frame_ff_lit     (relight;wine;gate) relight.frame.mode = pathtrace + relight.post.enable on ff_lit under
#                            Wine (rl_post_frame_run.py): every upscaler switch renders at its render extent, upscales to
#                            the swapchain, finite; composite == the path tracer's radiance on the GPU buffer at 1 spp.
#   rl_post_frame_determinism (relight;wine) two runs bit-identical.
#   rl_post_frame_validation (relight;wine;gate) the host validation + synchronization validation layer: 0 messages.

add_executable(fuse_relight_post_tests test_post_cpu.cpp)
target_link_libraries(fuse_relight_post_tests PRIVATE fuse_relight_post fuse_relight_pathtrace fuse_relight_options)
target_include_directories(fuse_relight_post_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../pathtrace/tests")
_rl_pt_cxx23(fuse_relight_post_tests)
foreach(_suite composite mip_bias options upscaler_switch nan_guard ltm determinism zero_alloc)
    add_test(NAME rl_post_${_suite} COMMAND fuse_relight_post_tests ${_suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_post_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 900)
endforeach()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET relight_d3d9 AND TARGET rl_app_ff_lit AND TARGET fuse_relight_post_frame
   AND CMAKE_CROSSCOMPILING)
    get_filename_component(_rl_post_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
    get_filename_component(_rl_post_taptools "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/tap" ABSOLUTE)
    get_filename_component(_rl_post_rastertools "${CMAKE_CURRENT_SOURCE_DIR}/../../raster/tests" ABSOLUTE)
    if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
        set(_rl_post_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
    else()
        set(_rl_post_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
    endif()
    set(_rl_post_common "${CMAKE_CURRENT_SOURCE_DIR}/rl_post_frame_run.py" --tap-tools "${_rl_post_taptools}"
        --raster-tools "${_rl_post_rastertools}" --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
        --runner "${_rl_post_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_post_prefix}"
        --exe "$<TARGET_FILE:rl_app_ff_lit>" --app ff_lit)
    foreach(_cmd frame determinism validation)
        set(_name rl_post_frame_${_cmd})
        if(_cmd STREQUAL "frame")
            set(_name rl_post_frame_ff_lit)
        endif()
        add_test(NAME ${_name} COMMAND "${Python3_EXECUTABLE}" ${_rl_post_common} ${_cmd}
                 --out "${CMAKE_BINARY_DIR}/relight/post/${_cmd}")
        set(_labels "relight;wine;gate")
        if(_cmd STREQUAL "determinism")
            set(_labels "relight;wine")
        endif()
        set_tests_properties(${_name} PROPERTIES LABELS "${_labels}" SKIP_RETURN_CODE 77 TIMEOUT 1800
                                                 RESOURCE_LOCK fuse_relight_renderer_pe)
    endforeach()
endif()
