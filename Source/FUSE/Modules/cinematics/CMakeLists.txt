add_library(fuse_cinematics STATIC
    src/event.cpp
    src/interpolate.cpp
    src/track.cpp
    src/camera_track.cpp
    src/look_at.cpp
    src/sprite_track.cpp
    src/property_track.cpp
    src/audio_track.cpp
    src/event_track.cpp
    src/cue_queue.cpp
    src/group.cpp
    src/playhead.cpp
    src/timeline.cpp
    include/fuse/cinematics/types.hpp
    include/fuse/cinematics/event.hpp
    include/fuse/cinematics/interpolate.hpp
    include/fuse/cinematics/track.hpp
    include/fuse/cinematics/camera_track.hpp
    include/fuse/cinematics/look_at.hpp
    include/fuse/cinematics/sprite_track.hpp
    include/fuse/cinematics/property_track.hpp
    include/fuse/cinematics/audio_track.hpp
    include/fuse/cinematics/event_track.hpp
    include/fuse/cinematics/cue_payload.hpp
    include/fuse/cinematics/cue_queue.hpp
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
