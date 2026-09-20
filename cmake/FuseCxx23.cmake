# C++23 pin for FUSE product targets (Track A P0).
# CXX_STANDARD 23 does not propagate. cxx_std_23 is PRIVATE so Legacy
# quarantine libs that PUBLIC-link fuse_core can stay on C++17 (Engine probe).
# Product subdirs still compile as 23 via CMAKE_CXX_STANDARD in Source/FUSE.

function(fuse_apply_cxx23 target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "fuse_apply_cxx23: '${target}' is not a CMake target")
    endif()
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_features(${target} PRIVATE cxx_std_23)
endfunction()

function(fuse_apply_cxx17 target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "fuse_apply_cxx17: '${target}' is not a CMake target")
    endif()
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_features(${target} PUBLIC cxx_std_17)
endfunction()
