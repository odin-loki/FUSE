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
)
target_include_directories(fuse_editor_controller PUBLIC Controller/include)
target_link_libraries(fuse_editor_controller PUBLIC fuse_editor_api)
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

add_library(fuse_editor_fuse STATIC Fuse/src/editor_shell.cpp Fuse/src/editor_shell_panels.cpp)
target_include_directories(fuse_editor_fuse PUBLIC Fuse/include)
target_link_libraries(fuse_editor_fuse PUBLIC fuse_editor_controller fuse_editor_scene_host fuse_app)
fuse_apply_cxx23(fuse_editor_fuse)

if(TARGET fuse_app_native)
    add_executable(fuse_editor_fuse_app Fuse/app/main.cpp)
    set_target_properties(fuse_editor_fuse_app PROPERTIES OUTPUT_NAME fuse_editor_fuse)
    target_link_libraries(fuse_editor_fuse_app PRIVATE fuse_editor_fuse fuse_app_native)
    fuse_apply_cxx23(fuse_editor_fuse_app)
endif()

# Twin gates: same names as the Qt editor gates (fuse_editor_qt_*), one executable, --gate=<name>; each writes a PNG.
add_executable(fuse_editor_ui_gates tests/test_editor_ui_gates.cpp)
target_link_libraries(fuse_editor_ui_gates PRIVATE fuse_editor_fuse)
if(TARGET fuse_app_native)
    target_link_libraries(fuse_editor_ui_gates PRIVATE fuse_app_native)
    target_compile_definitions(fuse_editor_ui_gates PRIVATE FUSE_EDITOR_UI_HAS_NATIVE=1)
endif()
fuse_apply_cxx23(fuse_editor_ui_gates)
set(FUSE_EDITOR_UI_GATES startup layout theme font_1 font_1_25 font_1_5 font_2 fuse_api wasd context_menu ui_frame
    idle_frames e15_console e15_transport e20_inspector e20_reparent e20_file_menu
    project_hub asset_browser material_editor sdf_sculpt profiler sequencer timeline scene_viewport gpu_viewport)
set(FUSE_EDITOR_UI_SHOTS "${CMAKE_BINARY_DIR}/editor_ui_shots")
file(MAKE_DIRECTORY "${FUSE_EDITOR_UI_SHOTS}")
foreach(gate ${FUSE_EDITOR_UI_GATES})
    add_test(NAME fuse_editor_ui_${gate} COMMAND fuse_editor_ui_gates --gate=${gate} --out=${FUSE_EDITOR_UI_SHOTS}/${gate}.png)
    set_tests_properties(fuse_editor_ui_${gate} PROPERTIES LABELS "fuse_ui;fuse_editor_ui")
endforeach()
# GPU gates: 77 = skipped (no Vulkan device / no display)
set_tests_properties(fuse_editor_ui_scene_viewport fuse_editor_ui_gpu_viewport PROPERTIES SKIP_RETURN_CODE 77)
