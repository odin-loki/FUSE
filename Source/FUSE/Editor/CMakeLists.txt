# WP-08 / U6 — editor API boundary (Qt-free) + optional Qt 6 shell.

add_library(fuse_editor_api STATIC
    src/command_queue.cpp
    src/editor_host.cpp
    include/fuse/editor/command_queue.hpp
    include/fuse/editor/editor_host.hpp
)

add_library(fuse::editor_api ALIAS fuse_editor_api)

target_include_directories(fuse_editor_api
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_editor_api PUBLIC fuse_core)
target_compile_features(fuse_editor_api PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()

if(FUSE_BUILD_EDITOR)
    if(FUSE_PLATFORM_MOBILE)
        message(WARNING "FUSE_BUILD_EDITOR is desktop-only; skipping fuse_editor on mobile profile")
    else()
        find_package(Qt6 QUIET COMPONENTS Widgets)
        if(Qt6_FOUND)
            set(FUSE_EDITOR_QT_AVAILABLE ON CACHE INTERNAL "Qt6 found for fuse_editor")

            add_executable(fuse_editor
                src/qt/main.cpp
                src/qt/main_window.cpp
                src/qt/main_window.hpp
                src/qt/project_hub_widget.cpp
                src/qt/project_hub_widget.hpp
                src/qt/viewport_placeholder_widget.cpp
                src/qt/viewport_placeholder_widget.hpp
                src/qt/game_loop_thread.cpp
                src/qt/game_loop_thread.hpp
            )

            target_link_libraries(fuse_editor
                PRIVATE
                    fuse_editor_api
                    Qt6::Widgets
            )

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
