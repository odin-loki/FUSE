# Quarantine + Engine probe TUs stay C++17 even when product dirs use 23.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fuse_legacy_common STATIC
    src/parallel_for_route.cpp
    src/string_intern_table.cpp
    src/sim_object_bridge.cpp
)

add_library(fuse::legacy_common ALIAS fuse_legacy_common)

target_include_directories(fuse_legacy_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_legacy_common PUBLIC fuse_core fuse_world2d)
fuse_apply_cxx17(fuse_legacy_common)

add_subdirectory(T3D)
add_subdirectory(T2D)
