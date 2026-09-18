add_executable(fuse_project_tests
    test_project.cpp
)

add_executable(fuse_asset_pipeline_tests
    test_asset_pipeline.cpp
)

add_executable(fuse_cook_cache_tests
    test_cook_cache.cpp
)

target_link_libraries(fuse_project_tests PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_asset_pipeline_tests PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_cook_cache_tests PRIVATE fuse_project fuse_core)
add_test(NAME fuse_project_loader COMMAND fuse_project_tests)
add_test(NAME fuse_assets_b79 COMMAND fuse_asset_pipeline_tests)
add_test(NAME fuse_cook_cache_b79 COMMAND fuse_cook_cache_tests)
