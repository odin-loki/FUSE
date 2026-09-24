# FUSE Relight RL-1.6 tests: vertex capture. Added from Source/FUSE/Relight/capture/vertex_capture/CMakeLists.txt.
#
#   rl_vertex_capture_unit          (gate;relight;capture) CPU: the dxso constant-range analysis and the
#                                   vertexshader hash (KATs against remix_hash_ref.py), the CPU back-transform
#                                   (1000 random draws within 1e-4, Remix's inverse / inverseAffine / normal
#                                   quirk), the capture ring, the SPIR-V pass on hand-built modules (every
#                                   variant and every rejection), the DXVK hand-over registry.
#   rl_vertex_capture_spirv_val     (relight;capture) the pass on glslang-compiled test shaders (SPIR-V 1.0
#                                   and 1.6) and the built-in variants; spirv-val (Vulkan 1.0 / 1.3 rules)
#                                   must accept every output. Skips (77) without spirv-val / glslang.
#   rl_vertex_capture_analysis_twin (relight;capture) the analysis on 2000 random D3D9 vertex shaders equals
#                                   the independent Python twin (rl_vs_analysis.py).
#   rl_vertex_capture_<app>         (relight;wine;capture) vs_sm2 / vs_sm3 through the Relight d3d9.dll with
#                                   relight.tap.mode = capture: every programmable-VS draw's captured
#                                   vertices back-transform to the vertex buffer's object-space positions
#                                   within 1e-4, the captured clip positions equal the CPU transform by the
#                                   app's VS constants, texcoord / normal / colour match, the vertexshader
#                                   component equals remix_hash_ref.py, every SPIR-V module DXVK compiled
#                                   (before and after the pass) passes spirv-val, and the back buffer is
#                                   bit-identical to FUSE_RELIGHT=0. Skips (77) without Wine / Xvfb.
# In a MinGW tree the C++ executable is PE and runs through CMAKE_CROSSCOMPILING_EMULATOR (Wine).

add_executable(fuse_relight_vertex_capture_tests test_vertex_capture.cpp)
target_link_libraries(fuse_relight_vertex_capture_tests PRIVATE fuse_relight_vertex_capture fuse_relight_vertex_capture_hook)
target_include_directories(fuse_relight_vertex_capture_tests SYSTEM PRIVATE "${_rl_vc_spirv_headers}")
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_vertex_capture_tests)
else()
    set_target_properties(fuse_relight_vertex_capture_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
add_test(NAME rl_vertex_capture_unit COMMAND fuse_relight_vertex_capture_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_vertex_capture_unit PROPERTIES LABELS "gate;relight;capture" TIMEOUT 300)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

get_filename_component(_rl_vc_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_vc_script "${CMAKE_CURRENT_SOURCE_DIR}/rl_vertex_capture.py")
set(_rl_vc_common "${Python3_EXECUTABLE}" "${_rl_vc_script}" --tool "$<TARGET_FILE:fuse_relight_vertex_capture_tests>")
if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
    foreach(_arg IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
        list(APPEND _rl_vc_common "--emulator=${_arg}")
    endforeach()
endif()
set(_rl_vc_out "${CMAKE_BINARY_DIR}/relight/vertex_capture")

# Host tools (find_program ignores the MinGW sysroot for absolute host paths).
find_program(FUSE_RELIGHT_SPIRV_VAL NAMES spirv-val)
find_program(FUSE_RELIGHT_VC_GLSLANG NAMES glslangValidator glslang)
set(_rl_vc_spv "")
if(FUSE_RELIGHT_VC_GLSLANG)
    foreach(_shader vc_basic vc_normal_out)
        foreach(_env vulkan1.0 vulkan1.3)
            set(_out "${CMAKE_CURRENT_BINARY_DIR}/spv/${_shader}_${_env}.spv")
            add_custom_command(
                OUTPUT "${_out}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/spv"
                COMMAND "${FUSE_RELIGHT_VC_GLSLANG}" -V --target-env ${_env} -o "${_out}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}.vert"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}.vert"
                COMMENT "RL-1.6 test shader ${_shader} (${_env})"
                VERBATIM)
            list(APPEND _rl_vc_spv "${_out}")
        endforeach()
    endforeach()
    add_custom_target(rl_vertex_capture_test_shaders ALL DEPENDS ${_rl_vc_spv})
endif()

add_test(NAME rl_vertex_capture_spirv_val
         COMMAND ${_rl_vc_common} spirv-val --spirv-val "${FUSE_RELIGHT_SPIRV_VAL}" --out "${_rl_vc_out}/spirv_val"
                 ${_rl_vc_spv})
set_tests_properties(rl_vertex_capture_spirv_val PROPERTIES LABELS "relight;capture" TIMEOUT 300 SKIP_RETURN_CODE 77)

add_test(NAME rl_vertex_capture_analysis_twin
         COMMAND ${_rl_vc_common} twin --out "${_rl_vc_out}/twin")
set_tests_properties(rl_vertex_capture_analysis_twin PROPERTIES LABELS "relight;capture" TIMEOUT 300)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_vc_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_vc_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
foreach(_app vs_sm2 vs_sm3)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_vertex_capture_${_app}
             COMMAND ${_rl_vc_common} run
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_vc_repo_root}/cmake/toolchains/fuse-wine-xvfb-run.sh"
                     --prefix-root "${_rl_vc_prefix}"
                     --ref "${_rl_vc_repo_root}/Tools/FUSE/Relight/remix_hash_ref.py"
                     --spirv-val "${FUSE_RELIGHT_SPIRV_VAL}"
                     --out "${_rl_vc_out}/${_app}")
    set_tests_properties(rl_vertex_capture_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()
