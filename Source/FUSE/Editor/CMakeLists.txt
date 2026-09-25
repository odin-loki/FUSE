# WP-08 / U6 — editor API boundary (Qt-free) + optional Qt 6 shell.

set(FUSE_EDITOR_API_SOURCES
    src/asset_browser.cpp
    src/command_queue.cpp
    src/command_stack.cpp
    src/console_panel.cpp
    src/editor_host.cpp
    src/editor_command_apply.cpp
    src/feature_pane_bridge.cpp
    src/ai_tree_profile_picker.cpp
    src/cinematics_seq_import.cpp
    src/viewport_seq_preview_stub.cpp
    src/runtime_viewport.cpp
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
    src/viewport_panel.cpp
    src/viewport_scene_view.cpp
    src/viewport_framebuffer.cpp
    src/entity_context_menu.cpp
    src/material_library_io.cpp
    src/profiler_flame_graph.cpp
)

set(FUSE_EDITOR_API_HEADERS
    include/fuse/editor/asset_browser.hpp
    include/fuse/editor/command_queue.hpp
    include/fuse/editor/command_stack.hpp
    include/fuse/editor/console_panel.hpp
    include/fuse/editor/editor_host.hpp
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
if(FUSE_BUILD_MODULES)
    target_link_libraries(fuse_editor_api PUBLIC fuse_ai fuse_cinematics)
endif()
fuse_apply_cxx23(fuse_editor_api)

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
    find_package(Qt6 QUIET COMPONENTS Gui)
    if(Qt6_FOUND AND TARGET Qt6::Gui)
        target_sources(fuse_editor_api PRIVATE src/viewport_vulkan_surface_qt.cpp)
        # PRIVATE so Qt cannot leak into fuse_core / fuse_runtime_smoke.
        target_link_libraries(fuse_editor_api PRIVATE Qt6::Gui)
        target_compile_definitions(fuse_editor_api PRIVATE FUSE_HAS_QT_VULKAN=1)
        message(STATUS "FUSE: fuse_editor_api Qt6 Gui Vulkan surface bootstrap enabled")
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()

set(FUSE_EDITOR_QT_AVAILABLE OFF CACHE INTERNAL "Qt6 found for fuse_editor")
if(FUSE_BUILD_EDITOR)
    if(FUSE_PLATFORM_MOBILE)
        message(WARNING "FUSE_BUILD_EDITOR is desktop-only; skipping fuse_editor on mobile profile")
    else()
        find_package(Qt6 QUIET COMPONENTS Widgets Gui)
        if(Qt6_FOUND AND TARGET Qt6::Widgets)
            set(FUSE_EDITOR_QT_AVAILABLE ON CACHE INTERNAL "Qt6 found for fuse_editor")

            # Qt host (B6.1): every panel / window class lives in fuse_editor_qt so the Qt gate
            # tests drive the same code the fuse_editor binary runs. Qt 6 widgets only (no immediate-mode UI)
            # (fuse_lint_b6_editor_qt6_only checks the link graph and includes).
            add_library(fuse_editor_qt STATIC
                src/qt/editor_panels.cpp
                src/qt/editor_panels.hpp
                src/qt/editor_theme.cpp
                src/qt/editor_theme.hpp
                src/qt/main_window.cpp
                src/qt/main_window.hpp
                src/qt/project_hub_widget.cpp
                src/qt/project_hub_widget.hpp
                src/qt/property_pane_widget.cpp
                src/qt/property_pane_widget.hpp
                src/qt/viewport_placeholder_widget.cpp
                src/qt/viewport_placeholder_widget.hpp
                src/qt/viewport_qt_vulkan_surface.cpp
                src/qt/viewport_qt_vulkan_surface.hpp
                src/qt/viewport_vulkan_window.cpp
                src/qt/viewport_vulkan_window.hpp
                src/qt/seq_preview_pane_widget.cpp
                src/qt/seq_preview_pane_widget.hpp
                src/qt/game_loop_thread.cpp
                src/qt/game_loop_thread.hpp
                src/qt/app_icon.cpp
                src/qt/app_icon.hpp
            )
            target_include_directories(fuse_editor_qt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/qt)
            target_link_libraries(fuse_editor_qt PUBLIC fuse_editor_api Qt6::Widgets Qt6::Gui)
            target_compile_definitions(fuse_editor_qt PUBLIC FUSE_HAS_QT=1 FUSE_EDITOR_HAS_QT_VULKAN=1)
            # The Qt viewport sources include <vulkan/vulkan.h> whenever Qt has QT_CONFIG(vulkan), even
            # with FUSE_BUILD_VULKAN off; on Windows the headers live in the Vulkan SDK, not the system.
            find_package(Vulkan QUIET)
            if(Vulkan_FOUND)
                target_include_directories(fuse_editor_qt PRIVATE ${Vulkan_INCLUDE_DIRS})
            endif()
            set_target_properties(fuse_editor_qt PROPERTIES AUTOMOC ON)
            fuse_apply_cxx23(fuse_editor_qt)

            # Editor UI font (Open Sans, Apache-2.0, vendored in Engine/lib/fonts/opensans) as a Qt resource.
            set(_fuse_editor_font "${FUSE_VENDOR_DIR}/fonts/opensans/OpenSans.ttf")
            set_source_files_properties("${_fuse_editor_font}" PROPERTIES QT_RESOURCE_ALIAS "OpenSans.ttf")
            qt_add_resources(fuse_editor_qt fuse_editor_fonts
                PREFIX "/fuse/editor/fonts"
                FILES "${_fuse_editor_font}"
            )

            # FUSE app icon (Appendix A "Icons"): Source/FUSE/Branding rasters as Qt resources for the
            # window icon; the Windows .rc embeds fuse.ico as the executable icon.
            set(_fuse_branding "${CMAKE_SOURCE_DIR}/Source/FUSE/Branding")
            set(_fuse_editor_icons)
            foreach(_px 16 32 48 256)
                set_source_files_properties("${_fuse_branding}/fuse_${_px}.png" PROPERTIES QT_RESOURCE_ALIAS "fuse_${_px}.png")
                list(APPEND _fuse_editor_icons "${_fuse_branding}/fuse_${_px}.png")
            endforeach()
            qt_add_resources(fuse_editor_qt fuse_editor_branding
                PREFIX "/fuse/branding"
                FILES ${_fuse_editor_icons}
            )

            add_executable(fuse_editor src/qt/main.cpp)
            if(WIN32)
                target_sources(fuse_editor PRIVATE "${_fuse_branding}/fuse_editor.rc")
            endif()
            target_link_libraries(fuse_editor PRIVATE fuse_editor_qt)
            fuse_apply_cxx23(fuse_editor)

            set_target_properties(fuse_editor PROPERTIES
                WIN32_EXECUTABLE ON
                MACOSX_BUNDLE ON
            )
        else()
            message(STATUS "FUSE_BUILD_EDITOR=ON but Qt6 Widgets not found — fuse_editor binary skipped (sources retained)")
        endif()
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests/qt)
endif()
