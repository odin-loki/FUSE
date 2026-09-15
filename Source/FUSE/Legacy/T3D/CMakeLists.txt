set(FUSE_T3D_LEGACY_PREFIX "fuse_t3d_" CACHE STRING "C symbol prefix for quarantined T3D globals")

add_library(fuse_t3d_legacy STATIC
    src/console_shim.cpp
    src/string_table_shim.cpp
    src/init.cpp
    src/scene_adapter.cpp
)

add_library(fuse::t3d_legacy ALIAS fuse_t3d_legacy)

target_include_directories(fuse_t3d_legacy
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(fuse_t3d_legacy
    PRIVATE
        FUSE_T3D_LEGACY_PREFIX=${FUSE_T3D_LEGACY_PREFIX}
)

target_link_libraries(fuse_t3d_legacy PUBLIC fuse_core fuse_legacy_common fuse_world3d)
target_compile_features(fuse_t3d_legacy PUBLIC cxx_std_17)
