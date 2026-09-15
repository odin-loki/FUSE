# fuse_animation — Track B7.1 skeletal animation scaffolding

set(FUSE_ANIMATION_PUBLIC_HEADERS
    include/fuse/animation/math.hpp
    include/fuse/animation/skeleton.hpp
    include/fuse/animation/clip.hpp
    include/fuse/animation/blend_tree.hpp
    include/fuse/animation/ik_solver.hpp
    include/fuse/animation/skinning.hpp
    include/fuse/animation/animator.hpp
)

set(FUSE_ANIMATION_SOURCES
    src/skeleton.cpp
    src/clip.cpp
    src/blend_tree.cpp
    src/ik_solver.cpp
    src/skinning.cpp
    src/animator.cpp
)

add_library(fuse_animation STATIC
    ${FUSE_ANIMATION_SOURCES}
    ${FUSE_ANIMATION_PUBLIC_HEADERS}
)

add_library(fuse::animation ALIAS fuse_animation)

target_include_directories(fuse_animation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_animation PUBLIC fuse_core fuse_ecs)
target_compile_features(fuse_animation PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
