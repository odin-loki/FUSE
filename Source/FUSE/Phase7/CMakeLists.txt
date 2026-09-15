# fuse_phase7 — Track B7.10 Phase 7 deliverables checklist + integration smoke

set(FUSE_PHASE7_PUBLIC_HEADERS
    include/fuse/phase7/phase7_test_registry.hpp
)

set(FUSE_PHASE7_SOURCES
    src/phase7_test_registry.cpp
)

add_library(fuse_phase7 STATIC
    ${FUSE_PHASE7_SOURCES}
    ${FUSE_PHASE7_PUBLIC_HEADERS}
)

add_library(fuse::phase7 ALIAS fuse_phase7)

target_include_directories(fuse_phase7
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_phase7
    PUBLIC
        fuse_core
        fuse_animation
        fuse_net
        fuse_terrain
        fuse_world_partition
        fuse_vfx
)

if(FUSE_BUILD_AUDIO)
    target_link_libraries(fuse_phase7 PUBLIC fuse_audio)
    target_compile_definitions(fuse_phase7 PUBLIC FUSE_PHASE7_HAS_AUDIO=1)
endif()

if(FUSE_BUILD_SCRIPT)
    target_link_libraries(fuse_phase7 PUBLIC fuse_script)
    target_compile_definitions(fuse_phase7 PUBLIC FUSE_PHASE7_HAS_SCRIPT=1)
endif()

if(FUSE_BUILD_PROJECT)
    target_link_libraries(fuse_phase7 PUBLIC fuse_project)
    target_compile_definitions(fuse_phase7 PUBLIC FUSE_PHASE7_HAS_PROJECT=1)
endif()

target_compile_features(fuse_phase7 PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
