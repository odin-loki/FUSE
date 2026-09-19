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

target_link_libraries(fuse_editor_api PUBLIC fuse_core fuse_ecs fuse_scene fuse_project)
if(FUSE_BUILD_MODULES)
    target_link_libraries(fuse_editor_api PUBLIC fuse_ai fuse_cinematics)
endif()
target_compile_features(fuse_editor_api PUBLIC cxx_std_17)

if(FUSE_BUILD_MODULES)
    target_link_libraries(fuse_editor_api PUBLIC fuse_ai)
endif()

if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_editor_api PUBLIC fuse_rhi fuse_hybrid)
    if(FUSE_VULKAN_BACKEND)
        target_compile_definitions(fuse_editor_api PUBLIC FUSE_VULKAN_BACKEND=1)
    endif()
    find_package(Qt6 QUIET COMPONENTS Gui)
    if(Qt6_FOUND AND TARGET Qt6::Gui)
        target_sources(fuse_editor_api PRIVATE src/viewport_vulkan_surface_qt.cpp)
        target_link_libraries(fuse_editor_api PRIVATE Qt6::Gui)
        target_compile_definitions(fuse_editor_api PRIVATE FUSE_HAS_QT_VULKAN=1)
        message(STATUS "FUSE: fuse_editor_api Qt6 Gui Vulkan surface bootstrap enabled")
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()

if(FUSE_BUILD_EDITOR)
    if(FUSE_PLATFORM_MOBILE)
        message(WARNING "FUSE_BUILD_EDITOR is desktop-only; skipping fuse_editor on mobile profile")
    else()
        find_package(Qt6 QUIET COMPONENTS Widgets Gui)
        if(Qt6_FOUND)
            set(FUSE_EDITOR_QT_AVAILABLE ON CACHE INTERNAL "Qt6 found for fuse_editor")

            add_executable(fuse_editor
                src/qt/main.cpp
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
                src/qt/game_loop_thread.cpp
                src/qt/game_loop_thread.hpp
            )

            target_link_libraries(fuse_editor
                PRIVATE
                    fuse_editor_api
                    Qt6::Widgets
            )
            if(TARGET Qt6::Gui)
                target_link_libraries(fuse_editor PRIVATE Qt6::Gui)
                target_compile_definitions(fuse_editor PRIVATE FUSE_EDITOR_HAS_QT_VULKAN=1)
            endif()

            target_compile_features(fuse_editor PRIVATE cxx_std_17)

            set_target_properties(fuse_editor PROPERTIES
                AUTOMOC ON
                WIN32_EXECUTABLE ON
                MACOSX_BUNDLE ON
            )
        else()
            message(STATUS "FUSE_BUILD_EDITOR=ON but Qt6 Widgets not found — fuse_editor binary skipped (sources retained)")
        endif()
    endif()
endif()
