add_executable(demo_fx main.cpp)
target_include_directories(demo_fx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
target_link_libraries(demo_fx PRIVATE fuse_fx fuse_project fuse_core)
target_compile_features(demo_fx PRIVATE cxx_std_17)
