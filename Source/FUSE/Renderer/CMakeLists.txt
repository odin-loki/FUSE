# fuse_rhi — Track B Vulkan bootstrap + render command list (B2.1) + shader scaffold (B2.4)

set(FUSE_RHI_PUBLIC_HEADERS
    include/fuse/renderer/atmosphere/atmosphere_params.hpp
    include/fuse/renderer/atmosphere/sky_lut.hpp
    include/fuse/renderer/atmosphere/sky_pass.hpp
    include/fuse/renderer/atmosphere/sky_scatter.hpp
    include/fuse/renderer/command_buffer.hpp
    include/fuse/renderer/composite_pass.hpp
    include/fuse/renderer/deferred/deferred_renderer.hpp
    include/fuse/renderer/deferred/frame_pipeline.hpp
    include/fuse/renderer/deferred/gbuffer.hpp
    include/fuse/renderer/lighting/clustered.hpp
    include/fuse/renderer/material/material.hpp
    include/fuse/renderer/material/material_system.hpp
    include/fuse/renderer/taa/taa_history.hpp
    include/fuse/renderer/taa/taa_jitter.hpp
    include/fuse/renderer/taa/taa_pass.hpp
    include/fuse/renderer/taa/taa_resolve.hpp
    include/fuse/renderer/taa/taa_types.hpp
    include/fuse/renderer/render_command_list.hpp
    include/fuse/renderer/render_graph.hpp
    include/fuse/renderer/renderer_bootstrap.hpp
    include/fuse/renderer/rhi_context.hpp
    include/fuse/renderer/resources.hpp
    include/fuse/renderer/resource_manager.hpp
    include/fuse/renderer/shader/shader_compiler.hpp
    include/fuse/renderer/shader/shader_io.hpp
    include/fuse/renderer/shader/shader_module.hpp
    include/fuse/renderer/shader/shader_types.hpp
    include/fuse/renderer/shadow/csm.hpp
    include/fuse/renderer/shadow/directional_shadow.hpp
    include/fuse/renderer/shadow/shadow_atlas.hpp
    include/fuse/renderer/shadow/shadow_pass.hpp
    include/fuse/renderer/vk/allocator.hpp
    include/fuse/renderer/vk/bindless.hpp
    include/fuse/renderer/vk/bootstrap.hpp
    include/fuse/renderer/vk/device.hpp
    include/fuse/renderer/vk/frame.hpp
    include/fuse/renderer/vk/instance.hpp
    include/fuse/renderer/vk/graphics_pipeline.hpp
    include/fuse/renderer/vk/pipeline_layout.hpp
    include/fuse/renderer/vk/raster_path.hpp
    include/fuse/renderer/vk/render_pass.hpp
    include/fuse/renderer/vk/surface.hpp
    include/fuse/renderer/vk/swapchain.hpp
    include/fuse/renderer/cuda/interop.hpp
    include/fuse/renderer/cuda/vk_sync.hpp
    include/fuse/renderer/cuda/stream_manager.hpp
)

set(FUSE_RHI_SOURCES
    src/atmosphere/sky_lut.cpp
    src/atmosphere/sky_pass.cpp
    src/atmosphere/sky_scatter.cpp
    src/command_buffer.cpp
    src/composite_pass.cpp
    src/deferred/deferred_renderer.cpp
    src/deferred/frame_pipeline.cpp
    src/deferred/gbuffer.cpp
    src/lighting/clustered_light_culler.cpp
    src/material/material.cpp
    src/material/material_system.cpp
    src/taa/taa_history.cpp
    src/taa/taa_jitter.cpp
    src/taa/taa_pass.cpp
    src/taa/taa_resolve.cpp
    src/render_command_list.cpp
    src/render_graph.cpp
    src/resource_manager.cpp
    src/renderer_bootstrap.cpp
    src/rhi_context.cpp
    src/shader/shader_compiler.cpp
    src/shader/shader_io.cpp
    src/shader/shader_module.cpp
    src/shadow/csm.cpp
    src/shadow/directional_shadow.cpp
    src/shadow/shadow_atlas.cpp
    src/shadow/shadow_pass.cpp
    src/vk/allocator.cpp
    src/vk/bindless.cpp
    src/vk/bootstrap.cpp
    src/vk/device.cpp
    src/vk/frame.cpp
    src/vk/instance.cpp
    src/vk/graphics_pipeline.cpp
    src/vk/pipeline_layout.cpp
    src/vk/raster_path.cpp
    src/vk/render_pass.cpp
    src/vk/surface.cpp
    src/vk/swapchain.cpp
    src/cuda/interop.cpp
    src/cuda/vk_sync.cpp
    src/cuda/stream_manager.cpp
)

add_library(fuse_rhi STATIC
    ${FUSE_RHI_SOURCES}
    ${FUSE_RHI_PUBLIC_HEADERS}
)

add_library(fuse::rhi ALIAS fuse_rhi)

target_include_directories(fuse_rhi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_rhi PUBLIC fuse_core)
target_compile_features(fuse_rhi PUBLIC cxx_std_17)
target_compile_definitions(fuse_rhi PRIVATE
    FUSE_SHADER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures"
)

# Optional VMA — stub allocator when header not vendored.
set(FUSE_VMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/VulkanMemoryAllocator/include")
if(EXISTS "${FUSE_VMA_INCLUDE_DIR}/vk_mem_alloc.h")
    target_include_directories(fuse_rhi PRIVATE "${FUSE_VMA_INCLUDE_DIR}")
    target_compile_definitions(fuse_rhi PRIVATE FUSE_VMA_AVAILABLE=1)
    message(STATUS "FUSE: VMA found at ${FUSE_VMA_INCLUDE_DIR}")
else()
    message(STATUS "FUSE: VMA not found — fuse_rhi uses stub GpuAllocator")
endif()

if(FUSE_VULKAN_BACKEND)
    target_link_libraries(fuse_rhi PUBLIC Vulkan::Vulkan)
    target_compile_definitions(fuse_rhi PUBLIC FUSE_VULKAN_BACKEND=1)
    message(STATUS "FUSE: fuse_rhi using Vulkan loader")
else()
    message(STATUS "FUSE: fuse_rhi built in stub mode (Vulkan loader not found)")
endif()

if(FUSE_CUDA_BACKEND)
    target_compile_definitions(fuse_rhi PUBLIC FUSE_HAS_CUDA=1)
    target_link_libraries(fuse_rhi PUBLIC CUDA::cudart)
endif()

option(FUSE_SHADER_GLSLANG "Enable runtime glslang shader compilation in fuse_rhi (optional; CI uses offline .spv fixtures)" OFF)
if(FUSE_SHADER_GLSLANG)
    find_package(glslang QUIET)
    if(glslang_FOUND)
        target_compile_definitions(fuse_rhi PRIVATE FUSE_SHADER_GLSLANG=1)
        target_link_libraries(fuse_rhi PRIVATE glslang::glslang glslang::SPIRV)
        message(STATUS "FUSE: fuse_rhi runtime glslang enabled")
    else()
        message(STATUS "FUSE: FUSE_SHADER_GLSLANG=ON but glslang not found — offline SPIR-V only")
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
