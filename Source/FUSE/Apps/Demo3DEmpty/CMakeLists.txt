add_executable(demo_3d_empty main.cpp)
target_include_directories(demo_3d_empty PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
target_link_libraries(demo_3d_empty PRIVATE fuse_hybrid fuse_project fuse_core)
target_compile_features(demo_3d_empty PRIVATE cxx_std_17)
