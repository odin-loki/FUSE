add_library(fuse_cinematics STATIC
    src/timeline.cpp
    include/fuse/cinematics/timeline.hpp
    include/fuse/cinematics/track.hpp
)

add_library(fuse::cinematics ALIAS fuse_cinematics)

target_include_directories(fuse_cinematics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_cinematics PUBLIC fuse_core fuse_world2d fuse_world3d)
target_compile_features(fuse_cinematics PUBLIC cxx_std_17)
