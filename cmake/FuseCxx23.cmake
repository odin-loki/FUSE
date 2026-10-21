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

# Disable floating-point multiply-add contraction so CPU reference kernels match GPU `precise` twins.
# clang-cl is CMAKE_CXX_COMPILER_ID Clang with MSVC true: never pass raw -ffp-contract=off to it.
function(fuse_no_fp_contract target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "fuse_no_fp_contract: '${target}' is not a CMake target")
    endif()
    set(_scope PRIVATE)
    if(ARGC GREATER 1)
        set(_scope ${ARGV1})
    endif()
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} ${_scope} /fp:precise /clang:-ffp-contract=off)
    elseif(MSVC)
        target_compile_options(${target} ${_scope} /fp:precise)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} ${_scope} -ffp-contract=off)
    endif()
endfunction()
