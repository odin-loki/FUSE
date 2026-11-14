# FUSE Remaster W1.7: the parity harness (docs/plans/FUSE_REMASTER_PLAN.md §4.5, §7 Wave 1). Added from
# Source/FUSE/Remaster/CMakeLists.txt; needs the track-B runtime (world_converter, World3D), the T3D trigger port
# (fuse_mechanics), the compat TorqueScript VM (fuse_compat_ts) and fuse_remaster_tscript (W1.6).
#
# Gate remaster_parity (remaster_parity_gate.cmake): Tests/fixtures/remaster/parity/door_trigger_pickup runs headless
# and its event log equals expected.log; a second run gives the same bytes; the negative scenario (the player
# turns back before the item) is reported as a mismatch.

foreach(_dep fuse_world3d fuse_world_converter fuse_project fuse_mechanics fuse_compat_ts fuse_remaster_tscript fuse_core)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Remaster: ${_dep} missing; remaster_parity not built")
        return()
    endif()
endforeach()

add_executable(fuse_remaster_parity remaster_parity.cpp)
target_link_libraries(fuse_remaster_parity PRIVATE fuse_world3d fuse_world_converter fuse_project fuse_mechanics fuse_compat_ts
                                                   fuse_remaster_tscript fuse_poco fuse_core)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_remaster_parity)
endif()
if(MSVC)
    target_compile_options(fuse_remaster_parity PRIVATE /utf-8)
endif()

if(FUSE_BUILD_CORE_TESTS OR BUILD_TESTING)
    string(REPLACE ";" "|" _rm_parity_emu "${CMAKE_CROSSCOMPILING_EMULATOR}")  # PE under MinGW: run via the emulator
    add_test(NAME remaster_parity
             COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:fuse_remaster_parity> "-DEMU=${_rm_parity_emu}"
                     -DFIXTURE=${FUSE_REMASTER_REPO_ROOT}/Tests/fixtures/remaster/parity/door_trigger_pickup
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/remaster_parity_gate
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/remaster_parity_gate.cmake)
    set_tests_properties(remaster_parity PROPERTIES LABELS "remaster;parity" TIMEOUT 300)
endif()
