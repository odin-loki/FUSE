# FUSE Relight RL-5.1 tests: path-tracing core, primary / PSR and the CPU reference path tracer. Added from
# ../CMakeLists.txt.
#
#   rl_pt_<suite>              (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree), see
#                              test_pt_cpu.cpp: furnace (white furnace: energy conservation of the RL-4.3 BSDFs under a
#                              uniform sky), lambert_sky (albedo x sky radiance), direct (closed forms: sphere, rect and
#                              emissive-triangle area lights, a delta distant light), mis (NEE-only / BSDF-only / MIS
#                              estimators agree), rr (Russian roulette unbiased), psr (mirror: G-buffer moved to the
#                              reflected surface, radiance unchanged, demodulation exact), alpha (legacy alpha test and
#                              blend), portal (a ray portal shows what its partner sees), capture (the RL-1.8 capture
#                              loader on a written capture), determinism (repeatable, CpuReference == CpuParallel),
#                              zero_alloc (steady-state scene update + reference frames).
# Not yet registered (open): the Lavapipe gates of PathTracerGpu (GPU vs CPU block parity on the Cornell scene and the
# test apps' captured scenes, PSR, determinism, zero allocation, validation).

add_executable(fuse_relight_pt_tests test_pt_cpu.cpp)
target_link_libraries(fuse_relight_pt_tests PRIVATE fuse_relight_pathtrace fuse_relight_options fuse_relight_mods_usd)
_rl_pt_cxx23(fuse_relight_pt_tests)
foreach(_suite furnace lambert_sky direct mis rr psr alpha portal capture determinism zero_alloc)
    add_test(NAME rl_pt_${_suite} COMMAND fuse_relight_pt_tests ${_suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_pt_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 900)
endforeach()

