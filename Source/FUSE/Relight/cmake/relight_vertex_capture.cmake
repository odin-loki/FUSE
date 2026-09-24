# RL-1.6 (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.5, §7): vertex capture as a SPIR-V pass.
# Owned by RL-1.6; included once from Source/FUSE/Relight/CMakeLists.txt, after RL-0.7's
# relight_renderer_mingw.cmake (so every target it hooks into exists).
#
#   capture/vertex_capture   fuse_relight_vertex_capture (CPU: SPIR-V pass, 48-byte layout, CPU
#                            back-transform, vertexshader hash component, capture ring),
#                            fuse_relight_vertex_capture_hook (the process-wide DXVK hand-over, linked
#                            into the DXVK DLLs), and in PE builds the GPU half compiled into the d3d9
#                            tap dispatcher.
#   tests/vertex_capture     rl_vertex_capture_unit, rl_vertex_capture_spirv_val,
#                            rl_vertex_capture_analysis_twin and, under Wine, rl_vertex_capture_<app>.
add_subdirectory(capture/vertex_capture)
