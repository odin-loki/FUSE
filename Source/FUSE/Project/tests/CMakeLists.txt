add_executable(fuse_project_tests
    test_project.cpp
)

target_link_libraries(fuse_project_tests PRIVATE fuse_project fuse_core)
add_test(NAME fuse_project_loader COMMAND fuse_project_tests)
