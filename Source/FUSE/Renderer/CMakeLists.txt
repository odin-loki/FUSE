# fuse_rhi — Track B Vulkan bootstrap + render command list (B2.1) + shader scaffold (B2.4)

set(FUSE_RHI_PUBLIC_HEADERS
    include/fuse/renderer/render_command_list.hpp
    include/fuse/renderer/rhi_context.hpp
    include/fuse/renderer/shader/shader_compiler.hpp
    include/fuse/renderer/shader/shader_io.hpp
    include/fuse/renderer/shader/shader_module.hpp
    include/fuse/renderer/shader/shader_types.hpp
    include/fuse/renderer/vk/bootstrap.hpp
    include/fuse/renderer/vk/device.hpp
    include/fuse/renderer/vk/instance.hpp
    include/fuse/renderer/vk/pipeline_layout.hpp
    include/fuse/renderer/vk/swapchain.hpp
)

set(FUSE_RHI_SOURCES
    src/render_command_list.cpp
    src/rhi_context.cpp
    src/shader/shader_compiler.cpp
    src/shader/shader_io.cpp
    src/shader/shader_module.cpp
    src/vk/bootstrap.cpp
    src/vk/device.cpp
    src/vk/instance.cpp
    src/vk/pipeline_layout.cpp
    src/vk/swapchain.cpp
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

if(FUSE_VULKAN_BACKEND)
    target_link_libraries(fuse_rhi PUBLIC Vulkan::Vulkan)
    target_compile_definitions(fuse_rhi PUBLIC FUSE_VULKAN_BACKEND=1)
    message(STATUS "FUSE: fuse_rhi using Vulkan loader")
else()
    message(STATUS "FUSE: fuse_rhi built in stub mode (Vulkan loader not found)")
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
