# Linux NGX bridge (docs/nvidia-plugin.md "Linux"). Included only when FUSE_ENABLE_NVIDIA_PLUGIN=ON,
# not WIN32, and FUSE_NVIDIA_DLSS_SDK_DIR points at the developer's own DLSS SDK checkout
# (https://github.com/NVIDIA/DLSS, NVIDIA RTX SDKs License). Never part of CI.
#
# The output libfuse_nvplugin_ngx.so statically links NVIDIA's libnvsdk_ngx.a: it is RTX-SDK-licensed
# object code. It lands in the build tree only; .gitignore and ctest fuse_nvidia_no_committed_binaries
# keep it (and libnvidia-ngx-dlss.so.*) out of the repository.

set(_ngx_sdk "${FUSE_NVIDIA_DLSS_SDK_DIR}")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_ngx_arch "Linux_aarch64")
else()
    set(_ngx_arch "Linux_x86_64")
endif()
set(_ngx_static "${_ngx_sdk}/lib/${_ngx_arch}/libnvsdk_ngx.a")
if(NOT EXISTS "${_ngx_sdk}/include/nvsdk_ngx_vk.h" OR NOT EXISTS "${_ngx_static}")
    message(WARNING "FUSE: FUSE_NVIDIA_DLSS_SDK_DIR=${_ngx_sdk} has no include/nvsdk_ngx_vk.h or "
                    "lib/${_ngx_arch}/libnvsdk_ngx.a — fuse_nvplugin_ngx not built")
    return()
endif()
if(NOT TARGET Vulkan::Vulkan AND NOT TARGET Vulkan::Headers)
    message(WARNING "FUSE: fuse_nvplugin_ngx needs the Vulkan headers — not built")
    return()
endif()

add_library(fuse_nvplugin_ngx MODULE "${CMAKE_CURRENT_LIST_DIR}/fuse_nvplugin_ngx.cpp")
target_link_libraries(fuse_nvplugin_ngx PRIVATE fuse_nvidia_plugin_abi "${_ngx_static}" ${CMAKE_DL_LIBS})
if(TARGET Vulkan::Headers)
    target_link_libraries(fuse_nvplugin_ngx PRIVATE Vulkan::Headers)
else()
    target_include_directories(fuse_nvplugin_ngx SYSTEM PRIVATE $<TARGET_PROPERTY:Vulkan::Vulkan,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# SYSTEM: NVIDIA's headers stay out of FUSE's warning flags.
target_include_directories(fuse_nvplugin_ngx SYSTEM PRIVATE "${_ngx_sdk}/include")
_fuse_nvidia_module(fuse_nvplugin_ngx "${FUSE_NVIDIA_PLUGIN_OUT}/ngx")
message(STATUS "FUSE: building fuse_nvplugin_ngx against ${_ngx_sdk} (RTX SDKs License; output never committed)")
