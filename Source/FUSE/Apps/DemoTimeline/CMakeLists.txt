add_executable(demo_timeline main.cpp)
target_include_directories(demo_timeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
target_link_libraries(demo_timeline PRIVATE fuse_cinematics fuse_project fuse_core)
target_compile_features(demo_timeline PRIVATE cxx_std_17)
