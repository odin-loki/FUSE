# E20 (MP-B6-QT-INSPECTOR / UNI-U6-INSP-1) headless gates: component schema (typed inspector
# fields), SetProperty / AddComponent / RemoveComponent through the CommandQueue with undo, and the
# hierarchy reparent that keeps the world pose.
add_executable(fuse_editor_e20_inspector_commands_tests test_e20_inspector_commands.cpp)
target_link_libraries(fuse_editor_e20_inspector_commands_tests PRIVATE fuse_editor_api fuse_core fuse_scene)
fuse_apply_cxx23(fuse_editor_e20_inspector_commands_tests)
add_test(NAME fuse_editor_e20_inspector_commands COMMAND fuse_editor_e20_inspector_commands_tests)
set_tests_properties(fuse_editor_e20_inspector_commands PROPERTIES LABELS "editor;e20" TIMEOUT 120)
