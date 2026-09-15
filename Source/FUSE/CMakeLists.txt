# FUSE product source umbrella — L0 fuse_core and future layers.

set(FUSE_CUDA_BACKEND FALSE)
if(FUSE_BUILD_CUDA)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(FUSE_CUDA_BACKEND TRUE)
        message(STATUS "FUSE: CUDA toolkit found — FUSE_HAS_CUDA enabled")
    else()
        message(STATUS "FUSE: FUSE_BUILD_CUDA=ON but toolkit not found — CUDA stub path only")
    endif()
endif()

add_subdirectory(Core)
add_subdirectory(ECS)
add_subdirectory(World2D)
add_subdirectory(World3D)

if(FUSE_BUILD_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(FUSE_VULKAN_BACKEND TRUE)
    else()
        set(FUSE_VULKAN_BACKEND FALSE)
        message(STATUS "FUSE: Vulkan loader not found — fuse_rhi stub backend")
    endif()
    add_subdirectory(Renderer)
endif()

add_subdirectory(Hybrid)

if(FUSE_BUILD_PROJECT)
    add_subdirectory(Project)
endif()

if(FUSE_BUILD_MODULES)
    add_subdirectory(Modules)
endif()

if(FUSE_BUILD_EDITOR_API OR FUSE_BUILD_EDITOR)
    add_subdirectory(Editor)
endif()

if(FUSE_BUILD_LEGACY)
    add_subdirectory(Legacy)
endif()

if(FUSE_BUILD_LEGACY AND FUSE_BUILD_SMOKE OR FUSE_BUILD_HYBRID_DEMO OR FUSE_BUILD_PARITY_DEMOS)
    add_subdirectory(Apps)
endif()
