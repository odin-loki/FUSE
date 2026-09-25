# FUSE Relight RL-5.7: post and upscale wiring (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8 steps 7-9, §5.6; Wave R5).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(render/post), after render/pathtrace.
# Owns Relight/render/post/**.
#
# Targets
#   fuse_relight_post        composite (composite.hpp), the local tone mapper Look node (local_tonemap.hpp), the
#                            rtx.conf upscaler / mip-bias configuration (post_config.hpp) and PostPipeline
#                            (post_pipeline.hpp: composite -> IUpscaler registry backend -> Look -> 8-bit output).
#   fuse_relight_post_frame  PtPostHook (pt_post.hpp), linked into RL-5.1's fuse_relight_pathtrace_frame with
#                            FUSE_RELIGHT_POST=1 so relight.post.enable routes the path tracer's frame through it.
# Tests: tests/ (rl_post_*).

foreach(_dep fuse_relight_pathtrace fuse_relight_options fuse_rhi fuse_fsr3)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-5.7 render/post skipped (no ${_dep})")
        return()
    endif()
endforeach()

add_library(fuse_relight_post STATIC
    src/post_config.cpp
    src/composite.cpp
    src/local_tonemap.cpp
    src/post_pipeline.cpp)
target_include_directories(fuse_relight_post PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_post PUBLIC fuse_relight_options fuse_rhi PRIVATE fuse_relight_pathtrace fuse_fsr3)
_rl_pt_cxx23(fuse_relight_post)

if(TARGET fuse_relight_pathtrace_frame AND TARGET fuse_relight_render_frame)
    add_library(fuse_relight_post_frame STATIC src/pt_post.cpp)
    target_link_libraries(fuse_relight_post_frame PUBLIC fuse_relight_post PRIVATE fuse_relight_render_frame fuse_fsr3)
    _rl_pt_cxx23(fuse_relight_post_frame)
    # The hook in pt_frame_renderer.cpp (#if defined(FUSE_RELIGHT_POST)).
    target_link_libraries(fuse_relight_pathtrace_frame PRIVATE fuse_relight_post_frame)
    target_compile_definitions(fuse_relight_pathtrace_frame PRIVATE FUSE_RELIGHT_POST=1)
endif()

add_subdirectory(tests)
