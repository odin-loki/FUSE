# FUSE Remaster W2.3 / W2.4: Remix-format interop (docs/plans/FUSE_REMASTER_PLAN.md §2.5, §7 Wave 2;
# docs/remaster/remix-format-notes.md). Added from Source/FUSE/CMakeLists.txt after Relight, only when FUSE Relight's
# mod importer exists (FUSE_BUILD_RELIGHT): the capture / mod layers are read with Relight's Remix-profile USD reader
# (vendor/tinyusdz) and importer, the DDS files with its reader; BC7 / BC5 come from the cook's encoders.
#
# Targets
#   fuse_remaster_remix      ingestRemix / exportRemix / checkExport + the CLI glue; linked into fuse_remaster, which
#                            then has `ingest-remix` and `export-remix` (FUSE_REMASTER_HAVE_REMIX)
#   remaster_remix_fixture   test helper: authors our replacement assets into a project, seeds lint violations
# Gate (ctest -L remaster): remaster_remix (remaster_remix_gate.cmake): the handcrafted capture fixture
#   (Tests/fixtures/remaster/remix/capture_basic) -> the expected oaid set with remix64 <-> sha256 links,
#   idempotent re-ingest; replacements -> export-remix -> validates, re-ingest finds every replacement, no
#   originals (lint, with seeded violations), recipe-only refused without --local, deterministic bytes.

foreach(_dep fuse_relight_mods_import fuse_relight_mods_usd fuse_relight_mods_assets fuse_cook_stubs fuse_remaster)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Remaster: ${_dep} missing; ingest-remix / export-remix not built")
        return()
    endif()
endforeach()

add_library(fuse_remaster_remix STATIC src/ingest.cpp src/export.cpp src/remaster_remix_cli.cpp)
target_include_directories(fuse_remaster_remix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_remaster_remix
    PUBLIC fuse_poco
    PRIVATE fuse_remaster_store fuse_remaster_db fuse_relight_mods_import fuse_relight_mods_usd fuse_relight_mods_assets
            fuse_cook_stubs)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_remaster_remix)
else()
    set_target_properties(fuse_remaster_remix PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
if(MSVC)
    target_compile_options(fuse_remaster_remix PRIVATE /utf-8 /permissive-)
endif()

target_link_libraries(fuse_remaster PRIVATE fuse_remaster_remix)
target_compile_definitions(fuse_remaster PRIVATE FUSE_REMASTER_HAVE_REMIX=1)

if(FUSE_BUILD_CORE_TESTS OR BUILD_TESTING)
    add_executable(remaster_remix_fixture tests/remix_fixture_tool.cpp)
    target_link_libraries(remaster_remix_fixture PRIVATE fuse_remaster_remix fuse_remaster_store fuse_poco)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(remaster_remix_fixture)
    endif()
    if(TARGET fuse_relight_mods_import_tests)
        get_filename_component(_rm_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
        add_test(NAME remaster_remix
                 COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:fuse_remaster> -DFIXTURE_TOOL=$<TARGET_FILE:remaster_remix_fixture>
                         -DSTAGER=$<TARGET_FILE:fuse_relight_mods_import_tests>
                         -DFIXTURE=${_rm_repo}/Tests/fixtures/remaster/remix/capture_basic
                         -DOUT=${CMAKE_CURRENT_BINARY_DIR}/remaster_remix_gate
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/remaster_remix_gate.cmake)
        set_tests_properties(remaster_remix PROPERTIES LABELS "remaster;remix" TIMEOUT 300)
    endif()
endif()
