# FORGE (https://github.com/odin-loki/FORGE) — Odin's OS layer, intrinsics and support library.
# FUSE consumes only FORGE's header-only layers (core, os, lane) through one INTERFACE target, `fuse_forge`,
# instead of add_subdirectory()-ing FORGE's full build (AI runtime, GPU compute, code generators).
#   -DFUSE_FORGE_DIR=/path/to/FORGE   use another checkout (default: vendor/forge submodule)
include_guard(GLOBAL)
get_filename_component(_fuse_forge_default "${CMAKE_CURRENT_LIST_DIR}/../vendor/forge" ABSOLUTE)
set(FUSE_FORGE_DIR "${_fuse_forge_default}" CACHE PATH "FORGE source root (header-only core/os/lane layers are used)")

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
endfunction()
