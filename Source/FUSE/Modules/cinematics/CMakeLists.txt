add_library(fuse_cinematics STATIC
    src/event.cpp
    src/track.cpp
    src/group.cpp
    src/playhead.cpp
    src/timeline.cpp
    include/fuse/cinematics/types.hpp
    include/fuse/cinematics/event.hpp
    include/fuse/cinematics/track.hpp
    include/fuse/cinematics/group.hpp
    include/fuse/cinematics/playhead.hpp
    include/fuse/cinematics/timeline.hpp
)

add_library(fuse::cinematics ALIAS fuse_cinematics)

target_include_directories(fuse_cinematics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_cinematics PUBLIC fuse_core fuse_world2d fuse_world3d)
target_compile_features(fuse_cinematics PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
