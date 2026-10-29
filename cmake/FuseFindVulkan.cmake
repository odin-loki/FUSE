# Prefer CMake's Vulkan package. On Windows without a SDK, use the system
# loader plus vendored Khronos headers so Win32 WSI can compile.

find_package(Vulkan QUIET)
if(TARGET Vulkan::Vulkan)
    set(FUSE_VULKAN_FOUND TRUE CACHE INTERNAL "FUSE located a Vulkan loader")
    return()
endif()

set(_fuse_vk_inc "${FUSE_VENDOR_DIR}/sdl/src/video/khronos")
if(NOT WIN32 OR NOT EXISTS "${_fuse_vk_inc}/vulkan/vulkan.h")
    return()
endif()
# The fallback links vulkan-1.dll directly, which only GNU ld (MinGW) accepts. link.exe / lld-link
# need an import library (vulkan-1.lib from the Vulkan SDK: set VULKAN_SDK, find_package above).
if(MSVC)
    message(STATUS "FUSE: no Vulkan SDK found (set VULKAN_SDK) — MSVC cannot link System32/vulkan-1.dll directly")
    return()
endif()
# Headers-only target for code that loads Vulkan through volk (cmake/FuseVolk.cmake), which needs no
# loader or import library at link time: vulkan-1.dll is opened at run time. Defined for MinGW
# whenever no SDK is found (the Relight MinGW renderer build uses it, RL-0.7); it does not by
# itself enable the Vulkan backend or fuse_core's Win32 WSI, which still key on Vulkan::Vulkan.
if(NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_fuse_vk_inc}")
endif()

# Candidates: an explicit FUSE_VULKAN_LOADER_DLL, then %WINDIR%\System32 (either case: a Wine prefix on a
# case-sensitive filesystem spells it "system32"), then, when cross-compiling, Wine's builtin PE loader.
set(FUSE_VULKAN_LOADER_DLL "" CACHE FILEPATH "vulkan-1.dll to link MinGW builds against (empty: search)")
set(_fuse_vk_cands "${FUSE_VULKAN_LOADER_DLL}")
foreach(_win IN ITEMS "$ENV{WINDIR}" "C:/Windows")
    if(NOT _win STREQUAL "")
        list(APPEND _fuse_vk_cands "${_win}/System32/vulkan-1.dll" "${_win}/system32/vulkan-1.dll")
    endif()
endforeach()
if(CMAKE_CROSSCOMPILING)
    list(APPEND _fuse_vk_cands
        "/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/vulkan-1.dll"
        "/usr/lib/wine/x86_64-windows/vulkan-1.dll"
        "/usr/local/lib/wine/x86_64-windows/vulkan-1.dll"
        "/opt/wine-stable/lib64/wine/x86_64-windows/vulkan-1.dll")
endif()
set(_fuse_vk_loader "")
foreach(_c IN LISTS _fuse_vk_cands)
    if(NOT _c STREQUAL "" AND EXISTS "${_c}")
        set(_fuse_vk_loader "${_c}")
        break()
    endif()
endforeach()
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
