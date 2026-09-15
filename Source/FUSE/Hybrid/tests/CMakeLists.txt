add_executable(fuse_hybrid_tests
    test_hybrid_composer.cpp
    test_frame_pipeline_mt.cpp
)

target_link_libraries(fuse_hybrid_tests PRIVATE fuse_hybrid fuse_core)
target_compile_features(fuse_hybrid_tests PRIVATE cxx_std_17)

add_test(NAME fuse_hybrid_tests COMMAND fuse_hybrid_tests)

if(FUSE_BUILD_VULKAN)
    add_executable(fuse_render_command_list test_render_command_list.cpp)
    target_link_libraries(fuse_render_command_list PRIVATE fuse_hybrid fuse_core)
    add_test(NAME fuse_render_command_list COMMAND fuse_render_command_list)

    add_executable(fuse_hybrid_renderer_bootstrap test_hybrid_renderer_bootstrap.cpp)
    target_link_libraries(fuse_hybrid_renderer_bootstrap PRIVATE fuse_hybrid fuse_core)
    add_test(NAME fuse_hybrid_renderer_bootstrap COMMAND fuse_hybrid_renderer_bootstrap)
endif()
