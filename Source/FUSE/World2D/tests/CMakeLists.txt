add_executable(fuse_world2d_tests
    test_world2d_frame.cpp
)

target_link_libraries(fuse_world2d_tests PRIVATE fuse_world2d fuse_core)
target_compile_features(fuse_world2d_tests PRIVATE cxx_std_17)

add_test(NAME fuse_world2d_tests COMMAND fuse_world2d_tests)

if(FUSE_BUILD_PROJECT)
    add_executable(fuse_world2d_fuselevel_bridge_tests
        test_fuselevel_bridge.cpp
    )

    target_link_libraries(fuse_world2d_fuselevel_bridge_tests PRIVATE fuse_world2d fuse_world_converter fuse_core fuse_project)
    add_test(NAME fuse_world2d_fuselevel_bridge COMMAND fuse_world2d_fuselevel_bridge_tests)
endif()
