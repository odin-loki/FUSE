# FUSE Remaster W1.1-W1.4 (docs/plans/FUSE_REMASTER_PLAN.md §7 Wave 1): fuse_legacy_ts, the Torque DTS / DSQ adapter.
#
#   fuse_legacy_ts                   synthetic Torque fixtures (W1.1), DTS / DSQ -> POCO (W1.2), POCO -> FMSH v2 /
#                                    .fusetex / .fusemat / .fskl + assetcheck descriptor (W1.3), T3D mission -> POCO
#                                    Level with POCO asset references (W1.4)
#   fuse_remaster_torque_fixtures    writes the W1.1 fixtures (or prints their manifest)
# Gates (ctest -L remaster): remaster_torque_fixtures, remaster_legacy_ts_import, remaster_legacy_ts_cook,
# remaster_world_converter_poco.

add_library(fuse_legacy_ts STATIC
    src/torque_fixtures.cpp
    src/dts_poco.cpp
    src/poco_cook.cpp
    src/mission_poco.cpp
)
target_include_directories(fuse_legacy_ts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_legacy_ts PUBLIC fuse_poco fuse_remaster_store fuse_remaster_db fuse_cook_stubs fuse_world_converter)
_fuse_remaster_cxx(fuse_legacy_ts)

add_executable(fuse_remaster_torque_fixtures tools/fuse_remaster_torque_fixtures.cpp)
target_link_libraries(fuse_remaster_torque_fixtures PRIVATE fuse_legacy_ts)
_fuse_remaster_cxx(fuse_remaster_torque_fixtures)

if(FUSE_BUILD_CORE_TESTS OR BUILD_TESTING)
    add_executable(remaster_legacy_ts_tests tests/test_legacy_ts.cpp)
    target_link_libraries(remaster_legacy_ts_tests PRIVATE fuse_legacy_ts fuse_remaster_sqlite3)
    _fuse_remaster_cxx(remaster_legacy_ts_tests)
    if(TARGET fuse_remaster_tscript)
        target_link_libraries(remaster_legacy_ts_tests PRIVATE fuse_remaster_tscript)
        target_compile_definitions(remaster_legacy_ts_tests PRIVATE FUSE_LEGACY_TS_HAS_TSCRIPT=1)
    endif()
    set(_fixtures "${FUSE_REMASTER_REPO_ROOT}/Tests/fixtures/remaster/torque")
    set(_work "${CMAKE_CURRENT_BINARY_DIR}/legacy_ts_tmp")
    add_test(NAME remaster_torque_fixtures COMMAND remaster_legacy_ts_tests fixtures "${_fixtures}" "${_work}/fixtures")
    add_test(NAME remaster_legacy_ts_import COMMAND remaster_legacy_ts_tests import "${_fixtures}" "${_work}/import")
    # fuse_assetcheck is defined later (cmake/FuseLintGates.cmake, included with the core tests); without it the
    # cook gate skips (77).
    set(_assetcheck "")
    if(FUSE_BUILD_CORE_TESTS)
        set(_assetcheck "$<TARGET_FILE:fuse_assetcheck>")
    endif()
    add_test(NAME remaster_legacy_ts_cook COMMAND remaster_legacy_ts_tests cook "${_fixtures}" "${_work}/cook" "${_assetcheck}")
    add_test(NAME remaster_world_converter_poco COMMAND remaster_legacy_ts_tests world "${_fixtures}" "${_work}/world")
    set_tests_properties(remaster_torque_fixtures remaster_legacy_ts_import PROPERTIES LABELS "remaster;legacy_ts")
    set_tests_properties(remaster_legacy_ts_cook PROPERTIES LABELS "remaster;legacy_ts;asset;cook" SKIP_RETURN_CODE 77)
    set_tests_properties(remaster_world_converter_poco PROPERTIES LABELS "remaster;legacy_ts;world3d")
    set_tests_properties(remaster_torque_fixtures remaster_legacy_ts_import remaster_legacy_ts_cook
                         remaster_world_converter_poco PROPERTIES TIMEOUT 300)
endif()
