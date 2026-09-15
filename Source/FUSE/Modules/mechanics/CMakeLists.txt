add_library(fuse_mechanics STATIC
    src/component.cpp
    src/interactable.cpp
    include/fuse/mechanics/interactable.hpp
    include/fuse/mechanics/component.hpp
)

add_library(fuse::mechanics ALIAS fuse_mechanics)

target_include_directories(fuse_mechanics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_mechanics PUBLIC fuse_core fuse_world3d)
target_compile_features(fuse_mechanics PUBLIC cxx_std_17)
