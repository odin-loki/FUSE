# Gates of the finishing items (included at the end of tests/CMakeLists.txt).

# Dock drag and drop: tear-off by title bar / tab, drop zones with preview, Escape / touch-cancel restore, touch path
fuse_ui_test(fuse_ui_dock_drag test_dock_drag.cpp)
foreach(g tear_off zones tabs escape touch)
    add_test(NAME fuse_ui_dock_drag_${g} COMMAND fuse_ui_dock_drag --gate=${g})
    set_tests_properties(fuse_ui_dock_drag_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()
