add_executable(fuse_navigation_tests
    test_grid_navmesh.cpp
)

target_link_libraries(fuse_navigation_tests PRIVATE fuse_navigation fuse_core)

set_target_properties(fuse_navigation_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_features(fuse_navigation_tests PRIVATE cxx_std_23)

add_test(NAME fuse_navigation_tests COMMAND fuse_navigation_tests)
