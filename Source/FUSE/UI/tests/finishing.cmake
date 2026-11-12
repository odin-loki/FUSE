# Gates of the finishing items (included at the end of tests/CMakeLists.txt).

# Dock drag and drop: tear-off by title bar / tab, drop zones with preview, Escape / touch-cancel restore, touch path
fuse_ui_test(fuse_ui_dock_drag test_dock_drag.cpp)
foreach(g tear_off zones tabs escape touch)
    add_test(NAME fuse_ui_dock_drag_${g} COMMAND fuse_ui_dock_drag --gate=${g})
    set_tests_properties(fuse_ui_dock_drag_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()

# Behaviour gates for the public widget functions no other gate called (audit of widgets.hpp against the tests)
fuse_ui_test(fuse_ui_catalogue_gaps test_catalogue_gaps.cpp)
foreach(g double_spinbox tabbar close_popups timeline_view viewport_overlay nodegraph_disconnect haptic_theme touch_hooks)
    add_test(NAME fuse_ui_gap_${g} COMMAND fuse_ui_catalogue_gaps --gate=${g})
    set_tests_properties(fuse_ui_gap_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()

# Golden of the two catalogue widgets test_widget_golden.cpp did not cover: double spin box, context menu
add_test(NAME fuse_ui_widget_golden_spinmenu COMMAND fuse_ui_widget_golden --scene=spinmenu)
set_tests_properties(fuse_ui_widget_golden_spinmenu PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Accessibility completeness: tree dumps, actions and the bridge per widget family
if(TARGET fuse_ui_a11y)
    fuse_ui_test(fuse_ui_a11y_families test_a11y_families.cpp)
    target_link_libraries(fuse_ui_a11y_families PRIVATE fuse_ui_a11y)
    foreach(g controls menus dialogs_live list tree table modelview grid tabs text pro bridge)
        add_test(NAME fuse_ui_a11y_family_${g} COMMAND fuse_ui_a11y_families --gate=${g})
        set_tests_properties(fuse_ui_a11y_family_${g} PROPERTIES LABELS "fuse_ui;gate")
    endforeach()
endif()
