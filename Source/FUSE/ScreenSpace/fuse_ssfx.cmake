# fuse_ssfx — Qt-/Vulkan-free CPU screen-space effect references (B5.7: HBAO, SSR, SSGI).
#
# Shared by fuse_rhi (Renderer, forwarding headers under fuse/renderer/ssfx/) and fuse_compute (CPU path of
# launch_ssao/launch_ssr/launch_ssgi). Depends on fuse_core only, so neither consumer depends on the other.
# Renderer and Compute are optional independently (FUSE_BUILD_VULKAN / FUSE_BUILD_CUDA), so both include this
# fragment and the first one defines the target.

if(NOT TARGET fuse_ssfx)
    add_library(fuse_ssfx STATIC
        ${CMAKE_CURRENT_LIST_DIR}/include/fuse/ssfx/ssfx_view.hpp
        ${CMAKE_CURRENT_LIST_DIR}/include/fuse/ssfx/hbao.hpp
        ${CMAKE_CURRENT_LIST_DIR}/include/fuse/ssfx/ssr.hpp
        ${CMAKE_CURRENT_LIST_DIR}/include/fuse/ssfx/ssgi.hpp
        ${CMAKE_CURRENT_LIST_DIR}/src/ssfx_view.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/hbao.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/ssr.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/ssgi.cpp
    )
    add_library(fuse::ssfx ALIAS fuse_ssfx)
    target_include_directories(fuse_ssfx
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    )
    target_link_libraries(fuse_ssfx PUBLIC fuse_core)
    target_compile_features(fuse_ssfx PUBLIC cxx_std_17)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(fuse_ssfx)
    endif()
endif()
