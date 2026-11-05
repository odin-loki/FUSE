# UI9 (docs/plans/FUSE_EDITOR_QT_FIRST_PLAN.md): the Qt-free EditorController (Q1 seam) shared by both editor shells, and
# the FUSE UI shell (fuse_editor_fuse) built beside the Qt one when FUSE_EDITOR_SHELL=fuse. Qt stays the default.
set(FUSE_EDITOR_SHELL "qt" CACHE STRING "Editor chrome toolkit: qt (fuse_editor_qt, default) or fuse (also builds fuse_editor_fuse + fuse_editor_ui_* gates)")
set_property(CACHE FUSE_EDITOR_SHELL PROPERTY STRINGS qt fuse)

if(NOT TARGET fuse_editor_api)
    return()
endif()

add_library(fuse_editor_controller STATIC
    Controller/src/editor_actions.cpp
    Controller/src/editor_controller.cpp
    Controller/src/editor_panel_models.cpp
    Controller/src/editor_project_tools.cpp
)
target_include_directories(fuse_editor_controller PUBLIC Controller/include)
target_link_libraries(fuse_editor_controller PUBLIC fuse_editor_api fuse_navigation)
fuse_apply_cxx23(fuse_editor_controller)

add_executable(fuse_editor_controller_tests tests/test_editor_controller.cpp)
target_link_libraries(fuse_editor_controller_tests PRIVATE fuse_editor_controller)
fuse_apply_cxx23(fuse_editor_controller_tests)
add_test(NAME fuse_editor_controller_api COMMAND fuse_editor_controller_tests)
set_tests_properties(fuse_editor_controller_api PROPERTIES LABELS "fuse_ui;fuse_editor_ui")

# EditorPanels (the panel models both shells drive): hub, assets, materials, sculpt, profiler, timeline, sequencer.
add_executable(fuse_editor_panel_models_tests tests/test_editor_panel_models.cpp)
target_link_libraries(fuse_editor_panel_models_tests PRIVATE fuse_editor_controller)
fuse_apply_cxx23(fuse_editor_panel_models_tests)
add_test(NAME fuse_editor_panel_models COMMAND fuse_editor_panel_models_tests)
set_tests_properties(fuse_editor_panel_models PROPERTIES LABELS "fuse_ui;fuse_editor_ui")

# Qt-independent scene renderer host (zero-copy viewport): EditorHost scene -> hybrid::HybridSceneRenderer on a device
# a swapchain presenter can adopt.
add_library(fuse_editor_scene_host STATIC SceneHost/src/scene_viewport_host.cpp)
target_include_directories(fuse_editor_scene_host PUBLIC SceneHost/include)
target_link_libraries(fuse_editor_scene_host PUBLIC fuse_editor_api)
fuse_apply_cxx23(fuse_editor_scene_host)

if(NOT FUSE_EDITOR_SHELL STREQUAL "fuse" OR NOT TARGET fuse_app)
    return()
endif()

# window icon rasters (Source/FUSE/Branding) embedded as byte arrays; regenerated when a PNG changes
set(_fuse_branding "${CMAKE_CURRENT_SOURCE_DIR}/../Branding")
set(_fuse_icons_hpp "${CMAKE_CURRENT_BINARY_DIR}/generated/fuse_icons_embedded.hpp")
add_custom_command(OUTPUT "${_fuse_icons_hpp}"
    COMMAND ${CMAKE_COMMAND} -DBRANDING=${_fuse_branding} -DOUT=${_fuse_icons_hpp} -P "${CMAKE_CURRENT_SOURCE_DIR}/Fuse/cmake/embed_icons.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Fuse/cmake/embed_icons.cmake" "${_fuse_branding}/fuse_16.png" "${_fuse_branding}/fuse_32.png"
            "${_fuse_branding}/fuse_48.png" "${_fuse_branding}/fuse_256.png"
    COMMENT "Embedding FUSE icon rasters")
add_library(fuse_editor_fuse STATIC Fuse/src/editor_shell.cpp Fuse/src/editor_shell_panels.cpp Fuse/src/editor_shell_tools.cpp
    Fuse/src/app_icon.cpp "${_fuse_icons_hpp}")
target_include_directories(fuse_editor_fuse PUBLIC Fuse/include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
get_filename_component(_fuse_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_include_directories(fuse_editor_fuse SYSTEM PRIVATE "${_fuse_repo_root}/vendor/stb")
target_link_libraries(fuse_editor_fuse PUBLIC fuse_editor_controller fuse_editor_scene_host fuse_app)
if(TARGET fuse_app_native)
    # OS file / colour dialogs (DialogServices) through the fuse_app platform layer
    target_link_libraries(fuse_editor_fuse PUBLIC fuse_app_native)
    target_compile_definitions(fuse_editor_fuse PRIVATE FUSE_EDITOR_FUSE_NATIVE=1)
endif()
fuse_apply_cxx23(fuse_editor_fuse)
if(FUSE_UI_ICON_PACK AND TARGET fuse_ui_icons)
    # toolbar icons: the starter pack cooked by fuse_ui_icon_cook
    target_compile_definitions(fuse_editor_fuse PRIVATE FUSE_EDITOR_ICON_PACK="${FUSE_UI_ICON_PACK}")
    add_dependencies(fuse_editor_fuse fuse_ui_icons)
endif()

if(TARGET fuse_app_native)
    add_executable(fuse_editor_fuse_app Fuse/app/main.cpp)
    # ARCHIVE_OUTPUT_NAME: the exe's import lib would otherwise be fuse_editor_fuse.lib, the library's own name (MSVC LNK1149).
    set_target_properties(fuse_editor_fuse_app PROPERTIES OUTPUT_NAME fuse_editor_fuse ARCHIVE_OUTPUT_NAME fuse_editor_fuse_app)
    target_link_libraries(fuse_editor_fuse_app PRIVATE fuse_editor_fuse fuse_app_native)
    if(TARGET fuse_app_a11y)
        target_link_libraries(fuse_editor_fuse_app PRIVATE fuse_app_a11y)
        target_compile_definitions(fuse_editor_fuse_app PRIVATE FUSE_EDITOR_HAS_A11Y=1)
    endif()
    if(WIN32)
        target_sources(fuse_editor_fuse_app PRIVATE "${_fuse_branding}/fuse_editor.rc") # executable icon (fuse.ico)
    endif()
    fuse_apply_cxx23(fuse_editor_fuse_app)
endif()

# Twin gates: same names as the Qt editor gates (fuse_editor_qt_*), one executable, --gate=<name>; each writes a PNG.
add_executable(fuse_editor_ui_gates tests/test_editor_ui_gates.cpp)
if(WIN32)
    target_sources(fuse_editor_ui_gates PRIVATE "${_fuse_branding}/fuse_editor.rc") # icon gate: executable icon resource
endif()
target_link_libraries(fuse_editor_ui_gates PRIVATE fuse_editor_fuse)
if(TARGET fuse_app_native)
    target_link_libraries(fuse_editor_ui_gates PRIVATE fuse_app_native)
    target_compile_definitions(fuse_editor_ui_gates PRIVATE FUSE_EDITOR_UI_HAS_NATIVE=1)
endif()
fuse_apply_cxx23(fuse_editor_ui_gates)
get_filename_component(_fuse_src_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_compile_definitions(fuse_editor_ui_gates PRIVATE FUSE_EDITOR_UI_SOURCE_DIR="${_fuse_src_root}")
if(TARGET fuse_cook)
    add_dependencies(fuse_editor_ui_gates fuse_cook) # project_settings / build_package cook through fuse_cook
endif()
if(FUSE_BUILD_PARITY_DEMOS AND FUSE_BUILD_PROJECT)
    # build_package: package + run the real demo_3d_empty player
    target_compile_definitions(fuse_editor_ui_gates PRIVATE FUSE_EDITOR_UI_DEMO_RUNTIME="$<TARGET_FILE:demo_3d_empty>")
    add_dependencies(fuse_editor_ui_gates demo_3d_empty)
endif()
set(FUSE_EDITOR_UI_GATES startup layout theme font_1 font_1_25 font_1_5 font_2 fuse_api wasd context_menu context_menu_dpr1_5 icon live_present view_modes preferences navigation terrain shortcut_rebind recent_projects entity_group colliders open_script ui_frame
    idle_frames e15_console e15_transport e20_inspector e20_reparent e20_file_menu e20_wizard project_open
    project_hub asset_browser asset_browser_polish material_editor sdf_sculpt profiler sequencer timeline timeline_nav play_frame multi_viewport viewport_gizmo scene_viewport gpu_viewport
    present_adopt project_settings build_package edit_tools lighting play_camera
    shell_docks shell_toolbar shell_undo shell_play shell_assets shell_entity shell_gizmo shell_material shell_sculpt shell_overlay
    shell_timeline shell_layout shell_profiler shell_project)
set(FUSE_EDITOR_UI_SHOTS "${CMAKE_BINARY_DIR}/editor_ui_shots")
file(MAKE_DIRECTORY "${FUSE_EDITOR_UI_SHOTS}")
foreach(gate ${FUSE_EDITOR_UI_GATES})
    add_test(NAME fuse_editor_ui_${gate} COMMAND fuse_editor_ui_gates --gate=${gate} --out=${FUSE_EDITOR_UI_SHOTS}/${gate}.png)
    set_tests_properties(fuse_editor_ui_${gate} PROPERTIES LABELS "fuse_ui;fuse_editor_ui")
endforeach()
# plan §6 / UI9 name of the ui_frame gate: the full 9-dock repaint budget (2 ms) on its own, not beside other tests
add_test(NAME fuse_editor_ui_frame COMMAND fuse_editor_ui_gates --gate=ui_frame --out=${FUSE_EDITOR_UI_SHOTS}/frame.png)
set_tests_properties(fuse_editor_ui_frame PROPERTIES LABELS "fuse_ui;fuse_editor_ui;gate" RUN_SERIAL TRUE)
# GPU gates: 77 = skipped (no Vulkan device / no display)
set_tests_properties(fuse_editor_ui_scene_viewport fuse_editor_ui_gpu_viewport fuse_editor_ui_live_present fuse_editor_ui_present_adopt
    PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(fuse_editor_ui_build_package fuse_editor_ui_edit_tools fuse_editor_ui_shell_timeline PROPERTIES TIMEOUT 600)
# Gates that open Samples/unification/demo_3d_empty: its worlds/example.fuselevel is gitignored and cooked by the
# fuse_sample_level_demo_3d_empty fixture (Tools/FUSE/CMakeLists.txt).
set_tests_properties(fuse_editor_ui_project_settings fuse_editor_ui_build_package fuse_editor_ui_lighting
    fuse_editor_ui_play_camera fuse_editor_ui_shell_assets fuse_editor_ui_shell_project
    PROPERTIES FIXTURES_REQUIRED fuse_sample_level_demo_3d_empty)

# Qt-removal blockers on the panel pages (docs/editor/QT_PARITY.md): Profiler flame graph, timeline key drag retime.
# Qt-removal blockers (docs/editor/QT_PARITY.md): floating panels as OS windows, drag to dock, hierarchy drag and drop
# and context menu, colour dialog, OS file dialogs (FUSE UI fallback), Tab order. float_window / native_dialogs need a
# display (77 without one; Linux: under Xvfb).
add_executable(fuse_editor_ui_parity_gates tests/test_editor_ui_parity.cpp)
target_link_libraries(fuse_editor_ui_parity_gates PRIVATE fuse_editor_fuse)
fuse_apply_cxx23(fuse_editor_ui_parity_gates)
foreach(gate dock_drag hierarchy_dnd hierarchy_menu color_dialog tab_order float_window native_dialogs)
    if(gate MATCHES "float_window|native_dialogs" AND FUSE_UI_XVFB_RUN AND NOT WIN32)
        add_test(NAME fuse_editor_ui_${gate} COMMAND ${FUSE_UI_XVFB_RUN} -a -s "-screen 0 1920x1080x24 +bs"
                 $<TARGET_FILE:fuse_editor_ui_parity_gates> --gate=${gate})
    else()
        add_test(NAME fuse_editor_ui_${gate} COMMAND fuse_editor_ui_parity_gates --gate=${gate} --out=${FUSE_EDITOR_UI_SHOTS}/${gate}.png)
    endif()
    set_tests_properties(fuse_editor_ui_${gate} PROPERTIES LABELS "fuse_ui;fuse_editor_ui" SKIP_RETURN_CODE 77 TIMEOUT 300)
endforeach()
set_tests_properties(fuse_editor_ui_float_window fuse_editor_ui_native_dialogs PROPERTIES RUN_SERIAL TRUE)

add_executable(fuse_editor_ui_panels2_gates tests/test_editor_ui_panels2.cpp)
target_link_libraries(fuse_editor_ui_panels2_gates PRIVATE fuse_editor_fuse)
fuse_apply_cxx23(fuse_editor_ui_panels2_gates)
foreach(gate profiler_flame timeline_drag)
    add_test(NAME fuse_editor_ui_${gate} COMMAND fuse_editor_ui_panels2_gates --gate=${gate} --out=${FUSE_EDITOR_UI_SHOTS}/${gate}.png)
    set_tests_properties(fuse_editor_ui_${gate} PROPERTIES LABELS "fuse_ui;fuse_editor_ui")
endforeach()

# Desktop accessibility (Qt-removal prerequisite): a UI Automation client reads the real editor window (menus, panel
# pages, inspector fields) and drives it through AccessKit. Windows only; 77 without AccessKit.
if(WIN32 AND TARGET fuse_app_a11y)
    add_executable(fuse_editor_ui_a11y_gate tests/test_editor_ui_a11y.cpp)
    target_link_libraries(fuse_editor_ui_a11y_gate PRIVATE fuse_editor_fuse fuse_app_a11y ole32 oleaut32)
    fuse_apply_cxx23(fuse_editor_ui_a11y_gate)
    add_test(NAME fuse_editor_ui_a11y COMMAND fuse_editor_ui_a11y_gate)
    set_tests_properties(fuse_editor_ui_a11y PROPERTIES LABELS "fuse_ui;fuse_editor_ui;a11y" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
endif()
# Linux twin: an AT-SPI client (libatspi) reads and drives the editor's X11 window through AccessKit, on a private
# session bus under Xvfb (as fuse_ui_a11y_linux). 77 without AccessKit, libatspi, a session bus or a display.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET fuse_app_a11y AND TARGET fuse_app_native)
    add_executable(fuse_editor_ui_a11y_linux_gate tests/test_editor_ui_a11y_linux.cpp)
    target_link_libraries(fuse_editor_ui_a11y_linux_gate PRIVATE fuse_editor_fuse fuse_app_native fuse_app_a11y ${CMAKE_DL_LIBS})
    target_include_directories(fuse_editor_ui_a11y_linux_gate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../UI/App/src") # D-Bus loader
    find_program(FUSE_UI_ATSPI_BUS_LAUNCHER at-spi-bus-launcher
        PATHS /usr/libexec /usr/lib/at-spi2-core /usr/libexec/at-spi2-core /usr/lib/at-spi2)
    if(FUSE_UI_ATSPI_BUS_LAUNCHER)
        target_compile_definitions(fuse_editor_ui_a11y_linux_gate PRIVATE FUSE_ATSPI_BUS_LAUNCHER="${FUSE_UI_ATSPI_BUS_LAUNCHER}")
    endif()
    fuse_apply_cxx23(fuse_editor_ui_a11y_linux_gate)
    find_program(FUSE_UI_XVFB_RUN xvfb-run)
    find_program(FUSE_UI_DBUS_RUN_SESSION dbus-run-session)
    if(FUSE_UI_XVFB_RUN AND FUSE_UI_DBUS_RUN_SESSION)
        add_test(NAME fuse_editor_ui_a11y_linux COMMAND ${FUSE_UI_DBUS_RUN_SESSION} -- ${FUSE_UI_XVFB_RUN} -a
                 -s "-screen 0 1920x1080x24 +bs" $<TARGET_FILE:fuse_editor_ui_a11y_linux_gate>)
    else()
        add_test(NAME fuse_editor_ui_a11y_linux COMMAND fuse_editor_ui_a11y_linux_gate)
    endif()
    set_tests_properties(fuse_editor_ui_a11y_linux PROPERTIES LABELS "fuse_ui;fuse_editor_ui;a11y" SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE TIMEOUT 180)
endif()
