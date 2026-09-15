add_library(fuse_fx STATIC
    src/fx_composer.cpp
    include/fuse/fx/fx_composer.hpp
)

add_library(fuse::fx ALIAS fuse_fx)

target_include_directories(fuse_fx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_fx PUBLIC fuse_core fuse_world2d fuse_world3d)
target_compile_features(fuse_fx PUBLIC cxx_std_17)
