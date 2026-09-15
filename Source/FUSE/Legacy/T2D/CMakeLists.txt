set(FUSE_T2D_LEGACY_PREFIX "fuse_t2d_" CACHE STRING "C symbol prefix for quarantined T2D globals")

add_library(fuse_t2d_legacy STATIC
    src/console_shim.cpp
    src/string_table_shim.cpp
    src/init.cpp
)

add_library(fuse::t2d_legacy ALIAS fuse_t2d_legacy)

target_include_directories(fuse_t2d_legacy
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(fuse_t2d_legacy
    PRIVATE
        FUSE_T2D_LEGACY_PREFIX=${FUSE_T2D_LEGACY_PREFIX}
)

target_link_libraries(fuse_t2d_legacy PUBLIC fuse_core)
target_compile_features(fuse_t2d_legacy PUBLIC cxx_std_17)
