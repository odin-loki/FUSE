# AddressSanitizer application for fuse-asan / FUSE_*_ENABLE_ASAN.
# Flags are GNU/Clang (and MSVC /fsanitize=address) only. The fuse-asan
# preset still configures when libasan is missing — common on Windows MinGW —
# so fuse-debug is unaffected. Options stay ON; flags are skipped with STATUS.

set(FUSE_ASAN_SUPPORTED OFF)

if(FUSE_CORE_ENABLE_ASAN OR FUSE_SMOKE_ENABLE_ASAN)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(FUSE_ASAN_SUPPORTED ON)
        message(STATUS "FUSE: AddressSanitizer enabled (MSVC /fsanitize=address)")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        include(CheckCXXSourceCompiles)
        set(_fuse_asan_saved_req_flags "${CMAKE_REQUIRED_FLAGS}")
        set(_fuse_asan_saved_req_link "${CMAKE_REQUIRED_LINK_OPTIONS}")
        set(_fuse_asan_saved_quiet "${CMAKE_REQUIRED_QUIET}")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
        set(CMAKE_REQUIRED_LINK_OPTIONS ${CMAKE_REQUIRED_LINK_OPTIONS} -fsanitize=address)
        set(CMAKE_REQUIRED_QUIET TRUE)
        unset(FUSE_ASAN_LINK_OK CACHE)
        check_cxx_source_compiles("int main() { return 0; }" FUSE_ASAN_LINK_OK)
        set(CMAKE_REQUIRED_FLAGS "${_fuse_asan_saved_req_flags}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "${_fuse_asan_saved_req_link}")
        set(CMAKE_REQUIRED_QUIET "${_fuse_asan_saved_quiet}")
        if(FUSE_ASAN_LINK_OK)
            set(FUSE_ASAN_SUPPORTED ON)
            message(STATUS "FUSE: AddressSanitizer enabled (-fsanitize=address)")
        else()
            message(STATUS
                "FUSE: ASan requested but this toolchain cannot link libasan "
                "(common on Windows MinGW) — skipping sanitizer flags")
        endif()
    else()
        message(STATUS
            "FUSE: ASan requested but compiler is ${CMAKE_CXX_COMPILER_ID}; "
            "sanitizer flags are GNU/Clang-only — skipping")
    endif()
endif()

function(fuse_apply_asan target)
    if(NOT FUSE_SMOKE_ENABLE_ASAN OR NOT FUSE_ASAN_SUPPORTED)
        return()
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "fuse_apply_asan: '${target}' is not a CMake target")
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /fsanitize=address)
        return()
    endif()
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
endfunction()
