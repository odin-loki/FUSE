# fuse_rhi — Track B Vulkan bootstrap + render command list (B2.1) + shader scaffold (B2.4)

include("${CMAKE_SOURCE_DIR}/cmake/FuseShaderSpirvRegen.cmake")

set(FUSE_RHI_PUBLIC_HEADERS
    include/fuse/renderer/atmosphere/atmosphere_params.hpp
    include/fuse/renderer/atmosphere/sky_lut.hpp
    include/fuse/renderer/atmosphere/sky_pass.hpp
    include/fuse/renderer/atmosphere/sky_scatter.hpp
    include/fuse/renderer/atmosphere/sun_disk.hpp
    include/fuse/renderer/atmosphere/transmittance_lut.hpp
    include/fuse/renderer/command_buffer.hpp
    include/fuse/renderer/composite_pass.hpp
    include/fuse/renderer/deferred/deferred_renderer.hpp
    include/fuse/renderer/deferred/frame_pipeline.hpp
    include/fuse/renderer/deferred/gbuffer.hpp
    include/fuse/renderer/draw_list.hpp
    include/fuse/renderer/gi/ddgi.hpp
    include/fuse/renderer/gi/ddgi_kernels.hpp
    include/fuse/renderer/lighting/clustered.hpp
    include/fuse/renderer/postprocess/lens_flare.hpp
    include/fuse/renderer/volumetric/light_shafts.hpp
    include/fuse/renderer/volumetric/volumetric_fog.hpp
    include/fuse/renderer/material/material.hpp
    include/fuse/renderer/material/material_system.hpp
    include/fuse/renderer/postprocess/auto_exposure.hpp
    include/fuse/renderer/postprocess/bloom.hpp
    include/fuse/renderer/postprocess/color_grade.hpp
    include/fuse/renderer/postprocess/post_stack.hpp
    include/fuse/renderer/postprocess/tonemap.hpp
    include/fuse/renderer/postprocess/tonemap_curve.hpp
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
    include/fuse/renderer/shader/shader_watch.hpp
    include/fuse/renderer/shader/shader_io.hpp
    include/fuse/renderer/shader/shader_module.hpp
    include/fuse/renderer/shader/shader_types.hpp
    include/fuse/renderer/shadow/csm.hpp
    include/fuse/renderer/shadow/directional_shadow.hpp
    include/fuse/renderer/shadow/shadow_atlas.hpp
    include/fuse/renderer/shadow/shadow_pass.hpp
    include/fuse/renderer/vk/gpu_alloc_stats.hpp
    include/fuse/renderer/vk/allocator.hpp
    include/fuse/renderer/vk/bindless.hpp
    include/fuse/renderer/vk/composite_gpu_path.hpp
    include/fuse/renderer/vk/bootstrap.hpp
    include/fuse/renderer/vk/device.hpp
    include/fuse/renderer/vk/debug_utils.hpp
    include/fuse/renderer/vk/frame.hpp
    include/fuse/renderer/vk/fence_wait.hpp
    include/fuse/renderer/vk/instance.hpp
    include/fuse/renderer/vk/present_path.hpp
    include/fuse/renderer/vk/queue_submit.hpp
    include/fuse/renderer/vk/graphics_pipeline.hpp
    include/fuse/renderer/vk/compute_pipeline.hpp
    include/fuse/renderer/vk/pipeline_layout.hpp
    include/fuse/renderer/vk/pipeline_cache.hpp
    include/fuse/renderer/vk/image_readback.hpp
    include/fuse/renderer/vk/raster_path.hpp
    include/fuse/renderer/vk/render_pass.hpp
    include/fuse/renderer/vk/surface.hpp
    include/fuse/renderer/vk/swapchain.hpp
    include/fuse/renderer/vk/swapchain_util.hpp
    include/fuse/renderer/cuda/interop.hpp
    include/fuse/renderer/cuda/interop_fill.hpp
    include/fuse/renderer/cuda/vk_sync.hpp
    include/fuse/renderer/cuda/stream_manager.hpp
)

set(FUSE_RHI_SOURCES
    src/atmosphere/sky_lut.cpp
    src/atmosphere/sky_pass.cpp
    src/atmosphere/sky_scatter.cpp
    src/atmosphere/sun_disk.cpp
    src/atmosphere/transmittance_lut.cpp
    src/command_buffer.cpp
    src/composite_pass.cpp
    src/deferred/deferred_renderer.cpp
    src/deferred/frame_pipeline.cpp
    src/deferred/gbuffer.cpp
    src/draw_list.cpp
    src/gi/ddgi.cpp
    src/lighting/clustered_light_culler.cpp
    src/postprocess/lens_flare.cpp
    src/volumetric/light_shafts.cpp
    src/volumetric/volumetric_fog.cpp
    src/material/material.cpp
    src/material/material_system.cpp
    src/postprocess/auto_exposure.cpp
    src/postprocess/bloom.cpp
    src/postprocess/color_grade.cpp
    src/postprocess/post_stack.cpp
    src/postprocess/tonemap.cpp
    src/postprocess/tonemap_curve.cpp
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
    src/shader/shader_watch.cpp
    src/shader/shader_io.cpp
    src/shader/shader_module.cpp
    src/shadow/csm.cpp
    src/shadow/directional_shadow.cpp
    src/shadow/shadow_atlas.cpp
    src/shadow/shadow_pass.cpp
    src/vk/gpu_alloc_stats.cpp
    src/vk/allocator.cpp
    src/vk/bindless.cpp
    src/vk/composite_gpu_path.cpp
    src/vk/bootstrap.cpp
    src/vk/device.cpp
    src/vk/debug_utils.cpp
    src/vk/frame.cpp
    src/vk/fence_wait.cpp
    src/vk/instance.cpp
    src/vk/present_path.cpp
    src/vk/queue_submit.cpp
    src/vk/graphics_pipeline.cpp
    src/vk/compute_pipeline.cpp
    src/vk/pipeline_layout.cpp
    src/vk/pipeline_cache.cpp
    src/vk/image_readback.cpp
    src/vk/raster_path.cpp
    src/vk/render_pass.cpp
    src/vk/surface.cpp
    src/vk/swapchain.cpp
    src/vk/swapchain_util.cpp
    src/cuda/interop.cpp
    src/cuda/interop_fill.cpp
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

# GpuAllocator backend. Vulkan builds use the vendored, pinned VMA (Engine/lib/vma, see its VERSION
# file) by default; FUSE_RHI_USE_VMA=OFF keeps the native one-vkAllocateMemory-per-resource path as
# a fallback. Never picks up a system vk_mem_alloc.h. Stub builds (no Vulkan) use neither.
option(FUSE_RHI_USE_VMA "Back fuse_rhi GpuAllocator with vendored VMA (OFF = native vkAllocateMemory fallback)" ON)
set(FUSE_VMA_DIR "${CMAKE_SOURCE_DIR}/Engine/lib/vma")
if(FUSE_VULKAN_BACKEND AND FUSE_RHI_USE_VMA)
    if(NOT EXISTS "${FUSE_VMA_DIR}/include/vk_mem_alloc.h" OR NOT EXISTS "${FUSE_VMA_DIR}/VERSION")
        message(FATAL_ERROR "FUSE_RHI_USE_VMA=ON but vendored VMA is missing under ${FUSE_VMA_DIR} "
                            "(expected include/vk_mem_alloc.h + VERSION); configure with -DFUSE_RHI_USE_VMA=OFF "
                            "for the native allocator")
    endif()
    file(STRINGS "${FUSE_VMA_DIR}/VERSION" _fuse_vma_version_line REGEX "^version=")
    if(NOT _fuse_vma_version_line MATCHES "^version=([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR "${FUSE_VMA_DIR}/VERSION: no 'version=X.Y.Z' pin line")
    endif()
    set(FUSE_VMA_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FUSE_VMA_DIR}/VERSION")
    # SYSTEM: third-party header, keep FUSE warning levels off VMA's own code.
    target_include_directories(fuse_rhi SYSTEM PRIVATE "${FUSE_VMA_DIR}/include")
    target_compile_definitions(fuse_rhi
        PRIVATE
            FUSE_VMA_AVAILABLE=1
            FUSE_VMA_PIN_MAJOR=${CMAKE_MATCH_1}
            FUSE_VMA_PIN_MINOR=${CMAKE_MATCH_2}
            FUSE_VMA_PIN_PATCH=${CMAKE_MATCH_3}
            FUSE_VMA_VERSION_STRING="${FUSE_VMA_VERSION}"
        # Lets tests assert which allocator backend fuse_rhi was built with.
        PUBLIC
            FUSE_RHI_ALLOCATOR_VMA=1)
    message(STATUS "FUSE: fuse_rhi GpuAllocator uses vendored VMA ${FUSE_VMA_VERSION} (${FUSE_VMA_DIR})")
elseif(FUSE_VULKAN_BACKEND)
    message(STATUS "FUSE: fuse_rhi GpuAllocator uses native vkAllocateMemory (FUSE_RHI_USE_VMA=OFF)")
else()
    message(STATUS "FUSE: fuse_rhi GpuAllocator stub (Vulkan backend off)")
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
    target_sources(fuse_rhi PRIVATE kernels/interop_fill.cu)
    set_target_properties(fuse_rhi PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
endif()

if(FUSE_ENABLE_GLFW_PRESENT)
    if(FUSE_PLATFORM_WINDOW_GLFW)
        target_compile_definitions(fuse_rhi PUBLIC FUSE_ENABLE_GLFW_PRESENT=1)
        message(STATUS "FUSE: fuse_rhi desktop GLFW present path enabled")
    else()
        message(STATUS "FUSE: FUSE_ENABLE_GLFW_PRESENT=ON requires FUSE_PLATFORM_WINDOW_GLFW=ON")
    endif()
endif()

option(FUSE_ENABLE_WIN32_PRESENT "Allow vkQueuePresentKHR when Win32 WSI available (OFF for headless CI)" OFF)
if(FUSE_ENABLE_WIN32_PRESENT)
    get_target_property(_fuse_core_interface_defs fuse_core INTERFACE_COMPILE_DEFINITIONS)
    if(NOT _fuse_core_interface_defs)
        set(_fuse_core_interface_defs "")
    endif()
    if(FUSE_PLATFORM_WINDOW_WIN32 OR _fuse_core_interface_defs MATCHES "FUSE_PLATFORM_WINDOW_WIN32")
        target_compile_definitions(fuse_rhi PUBLIC FUSE_ENABLE_WIN32_PRESENT=1)
        message(STATUS "FUSE: fuse_rhi desktop Win32 present path enabled")
    else()
        message(STATUS "FUSE: FUSE_ENABLE_WIN32_PRESENT=ON requires FUSE_PLATFORM_WINDOW_WIN32")
    endif()
endif()

if(FUSE_ENABLE_QT_PRESENT)
    target_compile_definitions(fuse_rhi PUBLIC FUSE_ENABLE_QT_PRESENT=1)
    message(STATUS "FUSE: fuse_rhi desktop Qt present path enabled")
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

# B5 gate work streams: one fragment per topic (sources added to fuse_rhi + gate tests).
foreach(_fuse_b5_topic gbuffer_materials clustered shadows ddgi atmosphere post taa_ssfx)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/b5_${_fuse_b5_topic}.cmake")
endforeach()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/vk_followups.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/b5_rhi_rows.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/gpu_radix_sort.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_core_logic.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/upscale_quality.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/look.cmake")
# Optional, runtime-loaded NVIDIA DLSS / Streamline plugin seam + CI mocks (docs/nvidia-plugin.md).
add_subdirectory(plugins/nvidia)
# Upscaler abstraction (ITemporalUpscaler / ISpatialUpscaler, registry) + FSR1 / NIS / CAS ports (docs/upscalers.md).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/upscale.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp01.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_harness.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp03.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp04.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp02.cmake")  # WP-0.2 volk loader (cmake/FuseVolk.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp05.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp06.cmake")  # WP-0.6 GPU profiler (Tracy GPU zones) + RenderDoc capture
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp12.cmake")  # WP-1.2 meshlet cook (geometry/, Engine/lib/meshoptimizer)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp11.cmake")  # WP-1.1 GPU scene (include/fuse/renderer/gpu_scene, src/gpu_scene)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp13.cmake")  # WP-1.3 instance cull + Hi-Z + indirect count (culling/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp14.cmake")  # WP-1.4 visibility buffer (visbuffer/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp15.cmake")  # WP-1.5 material resolve (material_resolve/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp21.cmake")  # WP-2.1 GPU clustered lighting (lighting/gpu/, shaders/lighting/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp22.cmake")  # WP-2.2 LTC area lights + BRDF energy compensation (lighting/ltc/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp23.cmake")  # WP-2.3 forward transparency (forward/, shaders/forward/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp31.cmake")  # WP-3.1 virtual shadow map core (shadow/vsm/, core_logic/vsm_pages/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp32.cmake")  # WP-3.2 VSM raster, filtering, local lights (shadow/vsm_raster/, shaders/shadow_vsm/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp41.cmake")  # WP-4.1 GPU motion vectors + TAAU dispatch (temporal/, shaders/temporal/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp45.cmake")  # WP-4.5 GPU post stack (postprocess/gpu/, shaders/post/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp51.cmake")  # WP-5.1 mesh-shader path, T1 (meshlet/, shaders/meshlet/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp52.cmake")  # WP-5.2 cluster DAG + LOD (geometry/dag/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp54.cmake")  # WP-5.4 compute software rasteriser (swraster/, shaders/swraster/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp53.cmake")  # WP-5.3 cluster streaming + residency (geometry_streaming/, core_logic/residency/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp60.cmake")  # WP-6.0 acceleration structures, T2 (rt/, shaders/rt/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp63.cmake")  # WP-6.3 screen-space fallback on Vulkan (ssfx_gpu/, shaders/ssfx/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp64.cmake")  # WP-6.4 in-tree denoiser, SVGF / A-SVGF (denoise/, shaders/denoise/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp62.cmake")  # WP-6.2 ray-traced shadows + reflections, T2 (rt_effects/, shaders/rt_effects/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp61.cmake")  # WP-6.1 DDGI on Vulkan, T0 global SDF + T2 ray query (gi/gpu/, shaders/ddgi/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp71.cmake")  # WP-7.1 light tree (light_tree/, shaders/light_tree/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp82.cmake")  # WP-8.2 Hillaire atmosphere on Vulkan (atmosphere/, shaders/atmosphere/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp42.cmake")  # WP-4.2 FSR 3.1 temporal upscaler (upscale_backends/fsr3/, Engine/lib/fidelityfx)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp81.cmake")  # WP-8.1 froxel volumetric fog on Vulkan (volumetric/gpu/, shaders/volumetric/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp83.cmake")  # WP-8.3 volumetric clouds on Vulkan (clouds/, shaders/clouds/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp72.cmake")  # WP-7.2 ReSTIR DI and GI, T2 (restir/, shaders/restir/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp44.cmake")  # WP-4.4 frame generation + latency (framegen/, present/latency/, Engine/lib/fidelityfx)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp93.cmake")  # WP-9.3 device-generated commands (vk/dgc/, shaders/dgc/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp92.cmake")  # WP-9.2 3D Gaussian splatting (gsplat/, shaders/gsplat/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp43.cmake")  # WP-4.3 Intel XeSS plugin, runtime-loaded libxess (plugins/intel_xess/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp64b.cmake") # WP-6.4b denoiser interface + NVIDIA NRD plugin (denoise/denoiser.hpp, plugins/nvidia_nrd/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp91.cmake")  # WP-9.1 neural runtime, CPU reference + portable/coopmat kernels (neural/, shaders/neural/)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp65.cmake")  # WP-6.5 radiance cascades, research (research/radiance_cascades/)
