# E15 (UNI-U6-FILE-1, MP-B6-QT-SCENE-FILES, MP-B6-EDITOR-SCRIPT-PIE, UNI-U6-CON-1) headless gates.

# Project manifest writer, .fuselevel v3 (ECS component blocks), EditorHost file commands + dirty flag.
add_executable(fuse_editor_e15_scene_files_tests test_e15_scene_files.cpp)
target_link_libraries(fuse_editor_e15_scene_files_tests PRIVATE fuse_editor_api fuse_core fuse_scene fuse_project)
fuse_apply_cxx23(fuse_editor_e15_scene_files_tests)
add_test(NAME fuse_editor_e15_scene_files COMMAND fuse_editor_e15_scene_files_tests)
set_tests_properties(fuse_editor_e15_scene_files PROPERTIES LABELS "editor;e15" TIMEOUT 120)

# Console Lua REPL + engine commands, scripts in play-in-editor, script hot-reload.
add_executable(fuse_editor_e15_console_pie_tests test_e15_console_pie.cpp)
target_link_libraries(fuse_editor_e15_console_pie_tests PRIVATE fuse_editor_api fuse_core fuse_scene)
fuse_apply_cxx23(fuse_editor_e15_console_pie_tests)
add_test(NAME fuse_editor_e15_console_pie COMMAND fuse_editor_e15_console_pie_tests)
set_tests_properties(fuse_editor_e15_console_pie PROPERTIES LABELS "editor;e15" TIMEOUT 120)
