# FUSE Relight licence and attribution gates (docs/plans/FUSE_REMIX_PORT_PLAN.md RL-0.1, §0.3, §0.4).
#
# Registers two self-validating ctest entries (labels gate;lint;relight;licence):
#   rl_licence_text_scan  NVIDIA proprietary licence titles / header phrases / clean-room file names
#                         and identifiers / MDL bodies, LGPL/GPL text and DXVK's LGPL
#                         mingw-directx-headers, under Source/FUSE/Relight, Tests/relight,
#                         Tools/FUSE/Relight and Engine/lib/{dxvk,dxbc-spirv,xxhash,gdeflate};
#   rl_binary_gate        NVIDIA RTX SDK / Intel XeSS runtime binaries (and any native binary in the
#                         Relight / vendored trees) tracked or addable by git, repository-wide.
# Both first prove, on fixtures generated at test time, that every seeded violation is caught and
# every near miss passes (tests/licence/rl_licence_selftest.cmake).
#
# Only needs the source tree (pure `cmake -P` scripts, no targets), so it is included from
# cmake/FuseLintGates.cmake; Source/FUSE/Relight/CMakeLists.txt may include it too (idempotent).

get_property(_rl_licence_gates_done GLOBAL PROPERTY _RL_LICENCE_GATES_REGISTERED)
if(_rl_licence_gates_done)
    return()
endif()
set_property(GLOBAL PROPERTY _RL_LICENCE_GATES_REGISTERED TRUE)

if(DEFINED FUSE_BUILD_CORE_TESTS AND NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

get_filename_component(_rl_licence_repo "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_rl_licence_driver "${CMAKE_CURRENT_LIST_DIR}/../tests/licence/rl_licence_selftest.cmake")
set(_rl_licence_scratch "${CMAKE_BINARY_DIR}/relight_licence")

add_test(NAME rl_licence_text_scan
    COMMAND "${CMAKE_COMMAND}" -DMODE=text "-DREPO=${_rl_licence_repo}"
            "-DSCRATCH=${_rl_licence_scratch}/text" -P "${_rl_licence_driver}")
add_test(NAME rl_binary_gate
    COMMAND "${CMAKE_COMMAND}" -DMODE=binary "-DREPO=${_rl_licence_repo}"
            "-DSCRATCH=${_rl_licence_scratch}/binary" -P "${_rl_licence_driver}")
set_tests_properties(rl_licence_text_scan rl_binary_gate PROPERTIES
    LABELS "gate;lint;relight;licence" TIMEOUT 300)
set_tests_properties(rl_binary_gate PROPERTIES SKIP_REGULAR_EXPRESSION "RL_BINARY_GATE_SKIP")
