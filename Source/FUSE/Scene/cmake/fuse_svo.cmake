# fuse_svo — the Scene sparse voxel octree (fuse::scene::SVO, B3.5 packed depth-first ray layout) as its
# own small library: the one SVO type shared by Scene and Physics (UNI-B4-VOX-1). fuse_scene links it
# publicly; fuse_physics links it for VoxelVolume storage / Voxel collision without depending on the
# rest of fuse_scene (which reaches fuse_physics through fuse_project -> fuse_hybrid -> fuse_world3d).
# Included from Source/FUSE/Physics/CMakeLists.txt (added before Scene, and built even when
# FUSE_BUILD_PROJECT is off).

if(TARGET fuse_svo)
    return()
endif()

set(_fuse_svo_dir "${CMAKE_CURRENT_LIST_DIR}/..")
add_library(fuse_svo STATIC
    ${_fuse_svo_dir}/src/svo.cpp
    ${_fuse_svo_dir}/src/svo_ray_cast.cpp
    ${_fuse_svo_dir}/include/fuse/scene/math.hpp
    ${_fuse_svo_dir}/include/fuse/scene/svo.hpp
    ${_fuse_svo_dir}/include/fuse/scene/svo_ray_kernel.hpp
    ${_fuse_svo_dir}/include/fuse/scene/svo_ray_device.hpp
)
add_library(fuse::svo ALIAS fuse_svo)
target_include_directories(fuse_svo PUBLIC $<BUILD_INTERFACE:${_fuse_svo_dir}/include>)
target_link_libraries(fuse_svo PUBLIC fuse_core)
# No floating-point contraction in the host DDA / sdf interpolation: the same flags the sources had inside
# fuse_scene (inherited there from fuse_light_tree), so the Scene SVO ray gates stay bit-identical and the
# host walk matches the --fmad=false device walk on every compiler.
if(MSVC)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(fuse_svo PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/fp:precise>
                                                $<$<COMPILE_LANGUAGE:CXX>:/clang:-ffp-contract=off>)
    else()
        target_compile_options(fuse_svo PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/fp:precise>)
    endif()
else()
    target_compile_options(fuse_svo PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=off>)
endif()

# Single-source "svo_ray_cast" kernel (docs/compute-kernels.md): CUDA trampoline when the toolkit is found.
if(FUSE_CUDA_BACKEND)
    target_sources(fuse_svo PRIVATE ${_fuse_svo_dir}/kernels/svo_ray_cast.cu)
    set_target_properties(fuse_svo PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    # No fma contraction in the DDA: the device walk takes the same decisions as the host build.
    set_source_files_properties(${_fuse_svo_dir}/kernels/svo_ray_cast.cu PROPERTIES COMPILE_OPTIONS "-fmad=false")
endif()
unset(_fuse_svo_dir)
