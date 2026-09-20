add_executable(demo_timeline main.cpp)
target_include_directories(demo_timeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
target_link_libraries(demo_timeline PRIVATE fuse_demo_wiring fuse_cinematics fuse_scene fuse_world3d fuse_project fuse_core)
target_compile_features(demo_timeline PRIVATE cxx_std_17)
