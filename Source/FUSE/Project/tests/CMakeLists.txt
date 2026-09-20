add_executable(fuse_project_tests
    test_project.cpp
)

add_executable(fuse_world_converter_tests
    test_world_converter.cpp
)

add_executable(fuse_asset_pipeline_tests
    test_asset_pipeline.cpp
)

add_executable(fuse_cook_cache_tests
    test_cook_cache.cpp
)

add_executable(fuse_cook_dependency_graph_tests
    test_cook_dependency_graph.cpp
)

add_executable(fuse_parity_legacy_sources_tests
    test_parity_legacy_sources.cpp
)

add_executable(fuse_mission_load_tests
    test_mission_load.cpp
)

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" FUSE_SOURCE_DIR_CMAKE)

target_link_libraries(fuse_project_tests PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_world_converter_tests PRIVATE fuse_world_converter fuse_world2d fuse_core)
target_link_libraries(fuse_asset_pipeline_tests PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_cook_cache_tests PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_cook_dependency_graph_tests PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_parity_legacy_sources_tests PRIVATE fuse_world_converter fuse_project fuse_core)
target_link_libraries(fuse_mission_load_tests PRIVATE fuse_project fuse_core)
target_compile_definitions(fuse_mission_load_tests PRIVATE
    FUSE_SOURCE_DIR="${FUSE_SOURCE_DIR_CMAKE}"
)
add_test(NAME fuse_project_loader COMMAND fuse_project_tests)
add_test(NAME fuse_parity_legacy_sources COMMAND fuse_parity_legacy_sources_tests)
set_tests_properties(fuse_parity_legacy_sources PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(NAME fuse_mission_load_p4
    COMMAND fuse_mission_load_tests "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(fuse_mission_load_p4 PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(NAME fuse_world_converter COMMAND fuse_world_converter_tests)
add_test(NAME fuse_assets_b79 COMMAND fuse_asset_pipeline_tests)
add_test(NAME fuse_cook_cache_b79 COMMAND fuse_cook_cache_tests)
add_test(NAME fuse_cook_dependency_graph_b79 COMMAND fuse_cook_dependency_graph_tests)
