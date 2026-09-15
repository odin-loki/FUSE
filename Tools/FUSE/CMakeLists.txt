add_executable(fuse_import fuse_import.cpp)
add_executable(fuse_cook fuse_cook.cpp)

target_link_libraries(fuse_import PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_cook PRIVATE fuse_project fuse_core)
target_compile_features(fuse_import PRIVATE cxx_std_17)
target_compile_features(fuse_cook PRIVATE cxx_std_17)

if(FUSE_SMOKE_ENABLE_ASAN)
    target_compile_options(fuse_import PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fuse_import PRIVATE -fsanitize=address)
    target_compile_options(fuse_cook PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fuse_cook PRIVATE -fsanitize=address)
endif()
