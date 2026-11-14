# WP-08 / U6 — editor API boundary. The editor chrome is the FUSE UI shell in Source/FUSE/EditorShell
# (docs/plans/FUSE_EDITOR_QT_FIRST_PLAN.md §8: the former Qt 6 shell was removed at the Stage 3 switch).

set(FUSE_EDITOR_API_SOURCES
    src/asset_browser.cpp
    src/command_queue.cpp
    src/command_stack.cpp
    src/console_panel.cpp
    src/editor_host.cpp
    src/editor_prefab_links.cpp
    src/editor_command_apply.cpp
    src/editor_console_commands.cpp
    src/editor_scripting.cpp
    src/editor_scripting.hpp
    src/feature_pane_bridge.cpp
    src/ai_tree_profile_picker.cpp
    src/cinematics_seq_import.cpp
    src/viewport_seq_preview_stub.cpp
    src/runtime_viewport.cpp
    src/terrain_builder.cpp
    src/runtime_embed_session.cpp
    src/viewport_swapchain_wiring.cpp
    src/viewport_swapchain_recreate.cpp
    src/viewport_vulkan_surface.cpp
    src/gizmo_system.cpp
    src/hierarchy_model.cpp
    src/material_editor_panel.cpp
    src/material_property_binding.cpp
    src/material_property_inspect.cpp
    src/play_mode_controller.cpp
    src/play_session.cpp
    src/profiler_panel.cpp
    src/property_inspector.cpp
    src/sdf_sculpt_panel.cpp
    src/scene_hierarchy_panel.cpp
    src/undo_stack.cpp
    src/transform_gizmo.cpp
    src/material_catalog.cpp
    src/editor_timeline.cpp
    src/viewport_panel.cpp
    src/viewport_scene_view.cpp
    src/viewport_framebuffer.cpp
    src/entity_context_menu.cpp
    src/entity_clipboard.cpp
    src/entity_names.cpp
    src/material_library_io.cpp
    src/profiler_flame_graph.cpp
    src/component_schema.cpp
)

set(FUSE_EDITOR_API_HEADERS
    include/fuse/editor/asset_browser.hpp
    include/fuse/editor/terrain_builder.hpp
    include/fuse/editor/command_queue.hpp
    include/fuse/editor/command_stack.hpp
    include/fuse/editor/console_panel.hpp
    include/fuse/editor/editor_host.hpp
    include/fuse/editor/editor_console_commands.hpp
    include/fuse/editor/editor_scene.hpp
    include/fuse/editor/runtime_viewport.hpp
    include/fuse/editor/runtime_embed_session.hpp
    include/fuse/editor/viewport_swapchain_wiring.hpp
    include/fuse/editor/viewport_swapchain_recreate.hpp
    include/fuse/editor/editor_state.hpp
    include/fuse/editor/feature_pane_bridge.hpp
    include/fuse/editor/ai_tree_profile_picker.hpp
    include/fuse/editor/cinematics_seq_import.hpp
    include/fuse/editor/viewport_seq_preview_stub.hpp
    include/fuse/editor/gizmo_system.hpp
    include/fuse/editor/hierarchy_model.hpp
    include/fuse/editor/material_editor_panel.hpp
    include/fuse/editor/material_property_binding.hpp
    include/fuse/editor/material_property_inspect.hpp
    include/fuse/editor/play_mode_controller.hpp
    include/fuse/editor/play_session.hpp
    include/fuse/editor/profiler_panel.hpp
    include/fuse/editor/property_inspector.hpp
    include/fuse/editor/scene_hierarchy_panel.hpp
    include/fuse/editor/sdf_sculpt_panel.hpp
    include/fuse/editor/undo_stack.hpp
    include/fuse/editor/viewport_panel.hpp
    include/fuse/editor/viewport_vulkan_surface.hpp
    include/fuse/editor/viewport_scene_view.hpp
    include/fuse/editor/viewport_framebuffer.hpp
    include/fuse/editor/entity_context_menu.hpp
    include/fuse/editor/material_library_io.hpp
    include/fuse/editor/profiler_flame_graph.hpp
    include/fuse/editor/component_schema.hpp
)

add_library(fuse_editor_api STATIC
    ${FUSE_EDITOR_API_SOURCES}
    ${FUSE_EDITOR_API_HEADERS}
)

add_library(fuse::editor_api ALIAS fuse_editor_api)

target_include_directories(fuse_editor_api
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# fuse_physics depends only on fuse_core + fuse_ecs, so the editor can drive it in Play mode
# without a dependency cycle.
target_link_libraries(fuse_editor_api PUBLIC fuse_core fuse_ecs fuse_physics fuse_scene fuse_project fuse_world_converter)
# GAP-GAME-LOOP-ECS: PlaySession runs the World3D runtime schedule in PIE.
target_link_libraries(fuse_editor_api PUBLIC fuse_world3d)
# GUI-6: Terrain component -> heightfield / chunk mesh (terrain_builder.cpp).
target_link_libraries(fuse_editor_api PUBLIC fuse_terrain)
if(FUSE_BUILD_MODULES)
    target_link_libraries(fuse_editor_api PUBLIC fuse_ai fuse_cinematics)
endif()
fuse_apply_cxx23(fuse_editor_api)

# MP-B6-EDITOR-SCRIPT-PIE / UNI-U6-CON-1: console Lua REPL + scripts in PIE (fuse_script, E10/E12);
# the physics bridge adds the Physics.* script API and collision callbacks in PIE.
if(TARGET fuse_script)
    target_link_libraries(fuse_editor_api PUBLIC fuse_script)
    target_compile_definitions(fuse_editor_api PRIVATE FUSE_EDITOR_HAS_SCRIPT=1)
    if(TARGET fuse_script_physics)
        target_link_libraries(fuse_editor_api PUBLIC fuse_script_physics)
        target_compile_definitions(fuse_editor_api PRIVATE FUSE_EDITOR_HAS_SCRIPT_PHYSICS=1)
    endif()
endif()

if(FUSE_BUILD_MODULES)
    target_link_libraries(fuse_editor_api PUBLIC fuse_ai)
endif()

if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_editor_api PUBLIC fuse_rhi fuse_hybrid)
    # fuse_rhi's CPU-side systems (MaterialSystem, RasterPath stub) exist with or without a loader.
    target_compile_definitions(fuse_editor_api PUBLIC FUSE_EDITOR_HAS_RHI=1)
    if(FUSE_VULKAN_BACKEND)
        target_compile_definitions(fuse_editor_api PUBLIC FUSE_VULKAN_BACKEND=1)
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
