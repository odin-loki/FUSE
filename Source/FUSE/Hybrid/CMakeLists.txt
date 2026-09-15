add_library(fuse_hybrid STATIC
    src/hybrid_composer.cpp
    src/placeholder_renderer.cpp
    include/fuse/hybrid/hybrid_composer.hpp
    include/fuse/hybrid/placeholder_renderer.hpp
    include/fuse/hybrid/project_flags.hpp
)

add_library(fuse::hybrid ALIAS fuse_hybrid)

target_include_directories(fuse_hybrid
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_hybrid PUBLIC fuse_world3d fuse_world2d fuse_core)
target_compile_features(fuse_hybrid PUBLIC cxx_std_17)

if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_hybrid PUBLIC fuse_rhi)
    target_compile_definitions(fuse_hybrid PUBLIC FUSE_HAS_VULKAN_RHI=1)
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
