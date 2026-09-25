# fuse_animation — Track B7.1 skeletal animation scaffolding

set(FUSE_ANIMATION_PUBLIC_HEADERS
    include/fuse/animation/math.hpp
    include/fuse/animation/skeleton.hpp
    include/fuse/animation/clip.hpp
    include/fuse/animation/blend_tree.hpp
    include/fuse/animation/ik_solver.hpp
    include/fuse/animation/retarget.hpp
    include/fuse/animation/skinning.hpp
    include/fuse/animation/skinning_kernel.hpp
    include/fuse/animation/animator.hpp
)

set(FUSE_ANIMATION_SOURCES
    src/skeleton.cpp
    src/clip.cpp
    src/blend_tree.cpp
    src/ik_solver.cpp
    src/retarget.cpp
    src/skinning.cpp
    src/animator.cpp
)

# Single-source skinning kernel (docs/compute-kernels.md): CUDA trampoline over the same body.
if(FUSE_CUDA_BACKEND)
    list(APPEND FUSE_ANIMATION_SOURCES kernels/skinning.cu)
endif()

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

if(FUSE_CUDA_BACKEND)
    target_link_libraries(fuse_animation PUBLIC CUDA::cudart)
    set_target_properties(fuse_animation PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
