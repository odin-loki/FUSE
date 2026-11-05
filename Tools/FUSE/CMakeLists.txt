add_executable(fuse_import fuse_import.cpp)
add_executable(fuse_cook fuse_cook.cpp Cook/fuselevel_cook_stub.cpp)
add_executable(fuse_convert fuse_convert.cpp)

target_link_libraries(fuse_import PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_cook PRIVATE fuse_project fuse_world_converter fuse_core)
target_link_libraries(fuse_convert PRIVATE fuse_world_converter fuse_core)
target_include_directories(fuse_cook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

include("${CMAKE_SOURCE_DIR}/cmake/FuseCxx23.cmake")
fuse_apply_cxx23(fuse_import)
fuse_apply_cxx23(fuse_cook)
fuse_apply_cxx23(fuse_convert)

if(COMMAND fuse_apply_asan)
    fuse_apply_asan(fuse_import)
    fuse_apply_asan(fuse_cook)
    fuse_apply_asan(fuse_convert)
endif()

# Samples/unification/demo_3d_empty/worlds/example.fuselevel is cooked from example.mis and gitignored, so a fresh
# checkout has none. This fixture cooks it before the gates that open the sample from the source tree
# (fuse_editor_ui_{lighting,shell_assets,build_package,shell_project,...}, fuse_scene_magic_gates) — they declare
# FIXTURES_REQUIRED fuse_sample_level_demo_3d_empty.
if(FUSE_BUILD_CORE_TESTS)
    add_test(NAME fuse_sample_level_demo_3d_empty
        COMMAND "${CMAKE_COMMAND}" "-DCONVERT=$<TARGET_FILE:fuse_convert>"
            "-DMIS=${CMAKE_SOURCE_DIR}/Samples/unification/demo_3d_empty/worlds/example.mis"
            "-DGOLDEN=${CMAKE_SOURCE_DIR}/Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis"
            "-DOUTPUT=${CMAKE_SOURCE_DIR}/Samples/unification/demo_3d_empty/worlds/example.fuselevel"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_sample_level.cmake")
    set_tests_properties(fuse_sample_level_demo_3d_empty PROPERTIES
        FIXTURES_SETUP fuse_sample_level_demo_3d_empty LABELS "fixture" TIMEOUT 120)
endif()

# WorldExtract: python world-model video -> 3D world tool (ctest fuse_world_extract_synthetic).
if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(WorldExtract)
    add_subdirectory(AssetGen)  # FUSE_ASSET_PLAN W0.9 generators (ctest asset_determinism)
endif()
