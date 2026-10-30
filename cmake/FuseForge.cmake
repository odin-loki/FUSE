# FORGE (https://github.com/odin-loki/FORGE) — Odin's OS layer, intrinsics and support library.
# FUSE consumes only FORGE's header-only layers (core, os, lane) through one INTERFACE target, `fuse_forge`,
# instead of add_subdirectory()-ing FORGE's full build (AI runtime, GPU compute, code generators).
#   -DFUSE_FORGE_DIR=/path/to/FORGE   use another checkout (default: vendor/forge submodule)
include_guard(GLOBAL)
get_filename_component(_fuse_forge_default "${CMAKE_CURRENT_LIST_DIR}/../vendor/forge" ABSOLUTE)
set(FUSE_FORGE_DIR "${_fuse_forge_default}" CACHE PATH "FORGE source root (header-only core/os/lane layers are used)")

# fuse_target_is_x86_64(<out>): TRUE when the C++ compiler targets x86_64 (asks the compiler, not the host).
function(fuse_target_is_x86_64 out)
    if(NOT DEFINED FUSE_TARGET_X86_64)
        include(CheckCXXSourceCompiles)
        check_cxx_source_compiles("#if !defined(__x86_64__) && !defined(_M_X64)\n#error not x86_64\n#endif\nint main() { return 0; }"
                                  FUSE_TARGET_X86_64)
    endif()
    set(${out} ${FUSE_TARGET_X86_64} PARENT_SCOPE)
endfunction()
# fuse_simd_flags(<out> avx2|avx512): per-source ISA flags for GCC/Clang (incl. clang-cl's GNU driver) or MSVC.
function(fuse_simd_flags out isa)
    if(MSVC)
        if(isa STREQUAL "avx2")
            set(${out} "/arch:AVX2" PARENT_SCOPE)
        else()
            set(${out} "/arch:AVX512" PARENT_SCOPE)
        endif()
    elseif(isa STREQUAL "avx2")
        set(${out} "-mavx2;-mfma;-mf16c" PARENT_SCOPE)
    else()
        set(${out} "-mavx512f;-mavx512bw;-mavx512dq;-mavx512vl;-mfma" PARENT_SCOPE)
    endif()
endfunction()

function(fuse_add_forge)
    if(TARGET fuse_forge)
        return()
    endif()
    if(NOT EXISTS "${FUSE_FORGE_DIR}/os/include/forge/os/os.hpp")
        message(FATAL_ERROR "FUSE: FORGE not found at ${FUSE_FORGE_DIR}. Run `git submodule update --init vendor/forge` "
                            "or pass -DFUSE_FORGE_DIR=<FORGE checkout>.")
    endif()
    add_library(fuse_forge INTERFACE)
    add_library(fuse::forge ALIAS fuse_forge)
    target_include_directories(fuse_forge SYSTEM INTERFACE
        "${FUSE_FORGE_DIR}/os/include" "${FUSE_FORGE_DIR}/core/include" "${FUSE_FORGE_DIR}/lane/include")
    target_compile_features(fuse_forge INTERFACE cxx_std_23)
    if(WIN32)
        target_link_libraries(fuse_forge INTERFACE ws2_32 bcrypt)
    elseif(UNIX)
        target_link_libraries(fuse_forge INTERFACE ${CMAKE_DL_LIBS})
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(fuse_forge INTERFACE Threads::Threads)
    message(STATUS "FUSE: FORGE OS layer from ${FUSE_FORGE_DIR}")
    # FORGE lane runtime (CPU feature detection + multiversioned kernels), built from FORGE's own sources so the
    # GUI never carries its own CPUID code. x86_64 only (GCC/Clang incl. mingw, and MSVC); elsewhere the header-only
    # vec is used at the compile-time baseline (NEON on aarch64) and detection is skipped. The target architecture
    # is probed from the compiler: cross toolchains often leave CMAKE_SYSTEM_PROCESSOR empty or as the host's.
    fuse_target_is_x86_64(_x64)
    if(_x64 AND NOT FUSE_FORGE_NO_LANE)
        set(_l "${FUSE_FORGE_DIR}/lane/src")
        add_library(fuse_forge_lane STATIC "${_l}/kernels_scalar.cpp" "${_l}/kernels_avx2.cpp"
            "${_l}/kernels_avx512.cpp" "${_l}/dispatch.cpp" "${_l}/tile_amx.cpp" "${_l}/tile_sme.cpp")
        fuse_simd_flags(_avx2 avx2)
        fuse_simd_flags(_avx512 avx512)
        set_source_files_properties("${_l}/kernels_avx2.cpp" PROPERTIES COMPILE_OPTIONS "${_avx2}")
        set_source_files_properties("${_l}/kernels_avx512.cpp" PROPERTIES COMPILE_OPTIONS "${_avx512}")
        target_link_libraries(fuse_forge_lane PUBLIC fuse_forge)
        set_target_properties(fuse_forge_lane PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if(MSVC)
            target_compile_options(fuse_forge_lane PRIVATE /w)
        else()
            target_compile_options(fuse_forge_lane PRIVATE -w)
        endif()
    endif()
endfunction()
