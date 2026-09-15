add_library(fuse_adventure STATIC
    src/inventory.cpp
    src/interaction.cpp
    include/fuse/adventure/inventory.hpp
    include/fuse/adventure/interaction.hpp
)

add_library(fuse::adventure ALIAS fuse_adventure)

target_include_directories(fuse_adventure
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_adventure PUBLIC fuse_core fuse_mechanics)
target_compile_features(fuse_adventure PUBLIC cxx_std_17)
