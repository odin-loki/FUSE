# FUSE Relight RL-4.2 tests. Added from ../CMakeLists.txt.
#
#   rl_raster_cpu_<suite>        (gate;relight) the CPU frame builder: tiers, material, build (native; PE through the
#                                Wine emulator in the MinGW tree).
#   rl_raster_tier_<app>_<tier>  (golden;wine;relight;tier_t0/t1/t2) in-process, d3d9.dll: relight.frame.mode = raster
#                                with FUSE_RENDER_TIER_MAX = 0 / 1 / 2 for ff_lit, ff_alpha and sky_ui_hud: the last
#                                frame's dump vs the committed golden (Tests/relight/golden/raster/<app>_<tier>.png) and
#                                the documented per-app checks (rl_raster_run.py: lit vs unlit regions, background =
#                                the clear colour, the emissive probe, alpha test, blend order / blend math relations,
#                                HUD on top, shadows from T1, T2 = T1 with the absent RT features reported degraded).
#   rl_raster_decal_ff_alpha     (wine;relight) rtx.decalTextures on ff_alpha's ramp: the decal pass (rl_raster_run.py).
#   rl_raster_fog_ff_fog         (wine;relight) D3D linear fog: the far wall is the fog colour (rl_raster_run.py).
#   rl_raster_determinism        (wine;relight) one app rendered twice: bit-identical dumps.
#   rl_raster_validation         (relight;wine;vulkan;gate) the three apps in raster mode with the host validation layer
#                                + synchronization validation: no message id beyond the capture tap alone.

add_executable(fuse_relight_raster_tests test_raster_cpu.cpp)
target_link_libraries(fuse_relight_raster_tests PRIVATE fuse_relight_render_raster)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_raster_tests)
endif()
foreach(_suite tiers material build)
    add_test(NAME rl_raster_cpu_${_suite} COMMAND fuse_relight_raster_tests ${_suite})
    set_tests_properties(rl_raster_cpu_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 300)
endforeach()

# ---- in-process (d3d9.dll under Wine) --------------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT (Python3_Interpreter_FOUND AND TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()
get_filename_component(_rl_raster_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(_rl_raster_runner "${_rl_raster_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh")
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_raster_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_raster_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
set(_rl_raster_out "${CMAKE_BINARY_DIR}/relight/raster")
set(_rl_raster_driver "${CMAKE_CURRENT_SOURCE_DIR}/rl_raster_run.py")
set(_rl_raster_golden "${_rl_raster_repo}/Tests/relight/golden/raster")
get_filename_component(_rl_raster_taptools "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/tap" ABSOLUTE)
set(_rl_raster_common --tap-tools "${_rl_raster_taptools}" --d3d9 "$<TARGET_FILE:relight_d3d9>"
                      --d3d8 "$<TARGET_FILE:relight_d3d8>" --runner "${_rl_raster_runner}"
                      --prefix-root "${_rl_raster_prefix}" --golden-dir "${_rl_raster_golden}")
foreach(_app ff_lit ff_alpha sky_ui_hud)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    foreach(_tier t0 t1 t2)
        add_test(NAME rl_raster_tier_${_app}_${_tier}
                 COMMAND "${Python3_EXECUTABLE}" "${_rl_raster_driver}" ${_rl_raster_common}
                         --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                         tier --tier ${_tier} --out "${_rl_raster_out}/${_app}_${_tier}")
        set_tests_properties(rl_raster_tier_${_app}_${_tier} PROPERTIES
            LABELS "relight;golden;wine;tier_${_tier}" SKIP_RETURN_CODE 77 TIMEOUT 600
            RESOURCE_LOCK fuse_relight_renderer_pe)
    endforeach()
endforeach()
if(TARGET rl_app_ff_alpha)
    add_test(NAME rl_raster_decal_ff_alpha
             COMMAND "${Python3_EXECUTABLE}" "${_rl_raster_driver}" ${_rl_raster_common}
                     --exe "$<TARGET_FILE:rl_app_ff_alpha>" --app ff_alpha
                     decal --out "${_rl_raster_out}/decal")
    set_tests_properties(rl_raster_decal_ff_alpha PROPERTIES LABELS "relight;wine" SKIP_RETURN_CODE 77 TIMEOUT 600
                                                             RESOURCE_LOCK fuse_relight_renderer_pe)
endif()
if(TARGET rl_app_ff_fog)
    add_test(NAME rl_raster_fog_ff_fog
             COMMAND "${Python3_EXECUTABLE}" "${_rl_raster_driver}" ${_rl_raster_common}
                     --exe "$<TARGET_FILE:rl_app_ff_fog>" --app ff_fog
                     fog --out "${_rl_raster_out}/fog")
    set_tests_properties(rl_raster_fog_ff_fog PROPERTIES LABELS "relight;wine" SKIP_RETURN_CODE 77 TIMEOUT 600
                                                         RESOURCE_LOCK fuse_relight_renderer_pe)
endif()
if(TARGET rl_app_sky_ui_hud)
    add_test(NAME rl_raster_determinism
             COMMAND "${Python3_EXECUTABLE}" "${_rl_raster_driver}" ${_rl_raster_common}
                     --exe "$<TARGET_FILE:rl_app_sky_ui_hud>" --app sky_ui_hud
                     determinism --out "${_rl_raster_out}/determinism")
    set_tests_properties(rl_raster_determinism PROPERTIES LABELS "relight;wine" SKIP_RETURN_CODE 77 TIMEOUT 600
                                                          RESOURCE_LOCK fuse_relight_renderer_pe)
endif()
set(_rl_raster_validation_exes "")
foreach(_app ff_lit ff_alpha sky_ui_hud)
    if(TARGET rl_app_${_app})
        list(APPEND _rl_raster_validation_exes --exe "$<TARGET_FILE:rl_app_${_app}>")
    endif()
endforeach()
if(_rl_raster_validation_exes)
    add_test(NAME rl_raster_validation
             COMMAND "${Python3_EXECUTABLE}" "${_rl_raster_driver}" ${_rl_raster_common}
                     ${_rl_raster_validation_exes} --app validation
                     validation --out "${_rl_raster_out}/validation")
    set_tests_properties(rl_raster_validation PROPERTIES
        LABELS "relight;wine;vulkan;gate" SKIP_RETURN_CODE 77 TIMEOUT 900 RESOURCE_LOCK fuse_relight_renderer_pe)
endif()
