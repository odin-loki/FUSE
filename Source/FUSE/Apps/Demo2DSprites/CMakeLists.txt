add_executable(demo_2d_sprites main.cpp)
target_include_directories(demo_2d_sprites PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
target_link_libraries(demo_2d_sprites PRIVATE fuse_hybrid fuse_project fuse_core)
target_compile_features(demo_2d_sprites PRIVATE cxx_std_17)
