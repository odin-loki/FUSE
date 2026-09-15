add_executable(fuse_import fuse_import.cpp)

target_link_libraries(fuse_import PRIVATE fuse_project fuse_core)
target_compile_features(fuse_import PRIVATE cxx_std_17)

if(FUSE_SMOKE_ENABLE_ASAN)
    target_compile_options(fuse_import PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fuse_import PRIVATE -fsanitize=address)
endif()
