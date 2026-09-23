# Prefer CMake's Vulkan package. On Windows without a SDK, use the system
# loader plus vendored Khronos headers so Win32 WSI can compile.

find_package(Vulkan QUIET)
if(TARGET Vulkan::Vulkan)
    set(FUSE_VULKAN_FOUND TRUE CACHE INTERNAL "FUSE located a Vulkan loader")
    return()
endif()

set(_fuse_vk_inc "${CMAKE_SOURCE_DIR}/Engine/lib/sdl/src/video/khronos")
if(NOT WIN32 OR NOT EXISTS "${_fuse_vk_inc}/vulkan/vulkan.h")
    return()
endif()
# The fallback links vulkan-1.dll directly, which only GNU ld (MinGW) accepts. link.exe / lld-link
# need an import library (vulkan-1.lib from the Vulkan SDK: set VULKAN_SDK, find_package above).
if(MSVC)
    message(STATUS "FUSE: no Vulkan SDK found (set VULKAN_SDK) — MSVC cannot link System32/vulkan-1.dll directly")
    return()
endif()

set(_fuse_vk_loader "")
if(DEFINED ENV{WINDIR} AND EXISTS "$ENV{WINDIR}/System32/vulkan-1.dll")
    set(_fuse_vk_loader "$ENV{WINDIR}/System32/vulkan-1.dll")
elseif(EXISTS "C:/Windows/System32/vulkan-1.dll")
    set(_fuse_vk_loader "C:/Windows/System32/vulkan-1.dll")
endif()
if(_fuse_vk_loader STREQUAL "")
    return()
endif()

add_library(Vulkan::Vulkan SHARED IMPORTED)
set_target_properties(Vulkan::Vulkan PROPERTIES
    IMPORTED_IMPLIB "${_fuse_vk_loader}"
    IMPORTED_LOCATION "${_fuse_vk_loader}"
    INTERFACE_INCLUDE_DIRECTORIES "${_fuse_vk_inc}"
)

set(FUSE_VULKAN_FOUND TRUE CACHE INTERNAL "FUSE located a Vulkan loader")
message(STATUS "FUSE: Vulkan via system loader + vendored Khronos headers")
