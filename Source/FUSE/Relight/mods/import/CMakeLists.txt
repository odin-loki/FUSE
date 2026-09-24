# FUSE Relight RL-3.2: the mod importer -> POCO + DB (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.4, §4.5, §4.8, Wave R3).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(mods/import). It also builds the CLI
# (Tools/FUSE/Relight/fuse_relight_import.cpp) and the package's tests (tests/).
#
# Targets
#   fuse_relight_mods_import  material table (§4.5, every AperturePBR Opacity / Translucent / Portal parameter),
#                             UsdGeomMesh + xform import, UsdLux light table, the importer (mods and captures) writing
#                             POCO records + blobs + DB rows in the RL-1.8 store-shim layout, the DB merge and the
#                             store verifier. Pure CPU: native and MinGW.
#   fuse_relight_import       CLI: `fuse_relight_import <mod> --game <id> --out <store>`, `fuse_relight_import verify`.
# Tests: tests/ (rl_mods_import_unit, rl_mods_import_fixture_<name>, rl_mods_import_fixture_count).

foreach(_dep fuse_relight_mods_usd fuse_relight_mods_assets fuse_relight_capture_export)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "mods/import needs ${_dep} (add_subdirectory(mods/import) after RL-1.8, RL-3.3 and RL-3.1)")
    endif()
endforeach()

add_library(fuse_relight_mods_import STATIC
    src/light_table.cpp
    src/material_table.cpp
    src/mesh_import.cpp
    src/mod_importer.cpp)
target_include_directories(fuse_relight_mods_import PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_mods_import
    PUBLIC fuse_relight_mods_usd fuse_relight_capture_export
    PRIVATE fuse_relight_mods_assets fuse_relight_hash)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_mods_import)
else()
    set_target_properties(fuse_relight_mods_import PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

get_filename_component(_rl_mi_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_executable(fuse_relight_import "${_rl_mi_repo}/Tools/FUSE/Relight/fuse_relight_import.cpp")
target_link_libraries(fuse_relight_import PRIVATE fuse_relight_mods_import)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_import)
else()
    set_target_properties(fuse_relight_import PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(tests)
