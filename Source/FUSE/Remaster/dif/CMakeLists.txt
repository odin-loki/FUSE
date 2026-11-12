# FUSE Remaster W1.5: fuse_legacy_dif, a clean-room Torque interior (.dif) reader plus the fixture writer
# (docs/plans/FUSE_REMASTER_PLAN.md §6, §7 Wave 1; the byte layout is documented in include/fuse/remaster/dif/dif.hpp).
# Added from Source/FUSE/Remaster/CMakeLists.txt (add_subdirectory(dif)); std-only C++23.
#
# Gate (ctest -L remaster): remaster_dif: synthetic interior written by the same library -> brush count and
# triangle area match, lightmaps ignored, deterministic bytes, versions 0..14, truncation / mutation robustness.

add_library(fuse_legacy_dif STATIC src/dif_reader.cpp src/dif_writer.cpp src/dif_model.cpp)
target_include_directories(fuse_legacy_dif PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(COMMAND _fuse_remaster_cxx)
    _fuse_remaster_cxx(fuse_legacy_dif)
else()
    set_target_properties(fuse_legacy_dif PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

if(FUSE_BUILD_CORE_TESTS OR BUILD_TESTING)
    add_executable(remaster_dif_tests tests/test_dif.cpp)
    target_link_libraries(remaster_dif_tests PRIVATE fuse_legacy_dif)
    if(COMMAND _fuse_remaster_cxx)
        _fuse_remaster_cxx(remaster_dif_tests)
    else()
        set_target_properties(remaster_dif_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
    add_test(NAME remaster_dif COMMAND remaster_dif_tests)
    set_tests_properties(remaster_dif PROPERTIES LABELS "remaster;dif" TIMEOUT 120)
    # `fuse_remaster dif <file>` on the generated fixture: 3 brushes, area 307, lightmaps skipped.
    if(TARGET fuse_remaster)
        add_test(NAME remaster_dif_cli
                 COMMAND ${CMAKE_COMMAND} -DFIXTURE_EXE=$<TARGET_FILE:remaster_dif_tests> -DEXE=$<TARGET_FILE:fuse_remaster>
                         -DWORK=${CMAKE_CURRENT_BINARY_DIR}/remaster_dif_cli -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/remaster_dif_cli_gate.cmake)
        set_tests_properties(remaster_dif_cli PROPERTIES LABELS "remaster;dif" TIMEOUT 120)
    endif()
endif()
