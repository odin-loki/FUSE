add_library(fuse_legacy_common STATIC
    src/parallel_for_route.cpp
)

add_library(fuse::legacy_common ALIAS fuse_legacy_common)

target_include_directories(fuse_legacy_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_legacy_common PUBLIC fuse_core)
target_compile_features(fuse_legacy_common PUBLIC cxx_std_17)

add_subdirectory(T3D)
add_subdirectory(T2D)
