# fuse_vfx — Track B7.7 VFX scaffolding

set(FUSE_VFX_PUBLIC_HEADERS
    include/fuse/vfx/vfx_desc.hpp
    include/fuse/vfx/particle_emitter.hpp
    include/fuse/vfx/particle_soa_ops.hpp
    include/fuse/vfx/particle_gpu.hpp
    include/fuse/vfx/effect_instance.hpp
    include/fuse/vfx/particle_system.hpp
)

set(FUSE_VFX_SOURCES
    src/particle_emitter.cpp
    src/particle_soa_ops.cpp
    src/particle_gpu.cpp
    src/effect_instance.cpp
    src/particle_system.cpp
)

add_library(fuse_vfx STATIC
    ${FUSE_VFX_SOURCES}
    ${FUSE_VFX_PUBLIC_HEADERS}
)

add_library(fuse::vfx ALIAS fuse_vfx)

target_include_directories(fuse_vfx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_vfx PUBLIC fuse_core)
target_compile_features(fuse_vfx PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
