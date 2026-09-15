# Optional Torque2D engine target via ExternalProject.
# T2D CMake assumes it is the top-level project (CMAKE_SOURCE_DIR paths), so we
# build it in an isolated binary dir instead of add_subdirectory().

function(fuse_configure_torque2d)
    set(_t2d_root "${CMAKE_SOURCE_DIR}/third_party/Torque2D")
    if(NOT EXISTS "${_t2d_root}/CMakeLists.txt")
        message(STATUS "FUSE: third_party/Torque2D not present — skipping FUSE_BUILD_T2D (run: git submodule update --init third_party/Torque2D)")
        set(FUSE_T2D_AVAILABLE OFF PARENT_SCOPE)
        return()
    endif()

    set(FUSE_T2D_AVAILABLE ON PARENT_SCOPE)

    include(ExternalProject)

    set(_t2d_args
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    )
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND _t2d_args -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    endif()
    if(ANDROID)
        list(APPEND _t2d_args -DANDROID_ABI=${ANDROID_ABI})
        list(APPEND _t2d_args -DANDROID_PLATFORM=${ANDROID_PLATFORM})
    endif()

    ExternalProject_Add(fuse_torque2d
        SOURCE_DIR "${_t2d_root}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/Torque2D"
        CMAKE_ARGS ${_t2d_args}
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
            "${CMAKE_BINARY_DIR}/third_party/Torque2D/Torque2D"
        EXCLUDE_FROM_ALL FALSE
    )

    add_custom_target(fuse_t2d_engine ALIAS fuse_torque2d)
    message(STATUS "FUSE: Torque2D configured as external project target fuse_torque2d")
endfunction()
