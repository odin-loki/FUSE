# Quarantined dialect script VMs. Not linked into fuse_core, fuse_runtime_smoke,
# fuse_editor_api, or Qt targets.

option(FUSE_BUILD_COMPAT_TS "Build quarantined dialect script VMs under Compat/" ON)
if(FUSE_SHIPPING)
    set(FUSE_BUILD_COMPAT_TS OFF CACHE BOOL "Build quarantined dialect script VMs under Compat/" FORCE)
endif()
if(NOT FUSE_BUILD_COMPAT_TS)
    return()
endif()

function(fuse_compat_apply_cxx23 target)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${target})
    else()
        set_target_properties(${target} PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
    endif()
endfunction()

add_library(fuse_compat_ts STATIC
    src/ts_vm.cpp
    include/fuse/compat/ts.hpp
)

add_library(fuse::compat_ts ALIAS fuse_compat_ts)

target_include_directories(fuse_compat_ts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(fuse_compat_ts PUBLIC FUSE_HAS_COMPAT_TS=1)
fuse_compat_apply_cxx23(fuse_compat_ts)

add_library(fuse_compat_ts_t3d STATIC
    src/ts_t3d.cpp
    include/fuse/compat/ts_t3d.hpp
)

add_library(fuse::compat_ts_t3d ALIAS fuse_compat_ts_t3d)

target_include_directories(fuse_compat_ts_t3d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_compat_ts_t3d PUBLIC fuse_compat_ts)
fuse_compat_apply_cxx23(fuse_compat_ts_t3d)

add_library(fuse_compat_ts_t2d STATIC
    src/ts_t2d.cpp
    include/fuse/compat/ts_t2d.hpp
)

add_library(fuse::compat_ts_t2d ALIAS fuse_compat_ts_t2d)

target_include_directories(fuse_compat_ts_t2d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_compat_ts_t2d PUBLIC fuse_compat_ts)
fuse_compat_apply_cxx23(fuse_compat_ts_t2d)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
