add_executable(demo_ai_bt main.cpp)
target_include_directories(demo_ai_bt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
target_link_libraries(demo_ai_bt PRIVATE fuse_ai fuse_project fuse_core)
target_compile_features(demo_ai_bt PRIVATE cxx_std_17)
