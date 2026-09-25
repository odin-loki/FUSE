# fuse_b7 — Track B7.10 deliverables checklist + cross-module integration smoke.

set(FUSE_B7_PUBLIC_HEADERS
    ${CMAKE_SOURCE_DIR}/Source/FUSE/Core/include/fuse/core/b7_test_registry.hpp
)

set(FUSE_B7_SOURCES
    ${CMAKE_SOURCE_DIR}/Source/FUSE/Core/src/core/b7_test_registry.cpp
)

add_library(fuse_b7 STATIC
    ${FUSE_B7_SOURCES}
    ${FUSE_B7_PUBLIC_HEADERS}
)

add_library(fuse::b7 ALIAS fuse_b7)

target_include_directories(fuse_b7
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Source/FUSE/Core/include>
)

target_link_libraries(fuse_b7
    PUBLIC
        fuse_core
        fuse_animation
        fuse_net
        fuse_terrain
        fuse_world_partition
        fuse_vfx
)

if(FUSE_BUILD_AUDIO)
    target_link_libraries(fuse_b7 PUBLIC fuse_audio)
    target_compile_definitions(fuse_b7 PUBLIC FUSE_B7_HAS_AUDIO=1)
endif()

if(FUSE_BUILD_SCRIPT)
    target_link_libraries(fuse_b7 PUBLIC fuse_script)
    target_compile_definitions(fuse_b7 PUBLIC FUSE_B7_HAS_SCRIPT=1)
endif()

if(FUSE_BUILD_PROJECT)
    target_link_libraries(fuse_b7 PUBLIC fuse_project)
    target_compile_definitions(fuse_b7 PUBLIC FUSE_B7_HAS_PROJECT=1)
endif()

fuse_apply_cxx23(fuse_b7)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
