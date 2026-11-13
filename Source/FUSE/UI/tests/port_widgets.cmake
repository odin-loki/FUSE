# Gates of the widget port items (docs/ui/archive/PORT_magical_bassi.md §3.12, PORT_gui_framework.md §3.7 / §3.12);
# included at the end of tests/CMakeLists.txt.

# Editor-facing widget API from claude/magical-bassi-815825 on main's dock model: dock splits (+ layout format 2) next
# to tab groups / floating docks / dock drags, tree line geometry, timeline clip selection and clip-drag reporting
fuse_ui_test(fuse_ui_dock_split test_dock_split.cpp)
foreach(g split split_groups tree_line timeline_clip)
    add_test(NAME fuse_ui_dock_split_${g} COMMAND fuse_ui_dock_split --gate=${g})
    set_tests_properties(fuse_ui_dock_split_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()

# Qt-parity options from the gui-framework branch (PORT_gui_framework.md §3.7): line edit echo modes / max length /
# placeholder / read-only / validators / input masks, completer, editable combo box
fuse_ui_test(fuse_ui_lineedit test_lineedit.cpp)
fuse_ui_test(fuse_ui_completer test_completer.cpp)
fuse_ui_test(fuse_ui_editcombo test_editcombo.cpp)
# spin box options: typed entry, prefix / suffix, special value, wrapping, adaptive step, correction, auto-repeat
fuse_ui_test(fuse_ui_spinbox test_spinbox.cpp)
set_tests_properties(fuse_ui_lineedit fuse_ui_completer fuse_ui_editcombo fuse_ui_spinbox PROPERTIES LABELS "fuse_ui;gate")

# QTabBar / QTabWidget options on main's tab bar: closable, movable, disabled tabs, scroll buttons, add / remove pages
fuse_ui_test(fuse_ui_tab_options test_tab_options.cpp)
foreach(g closable movable scroll pages)
    add_test(NAME fuse_ui_tab_options_${g} COMMAND fuse_ui_tab_options --gate=${g})
    set_tests_properties(fuse_ui_tab_options_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()

# Extras widgets from the gui-framework branch, now part of fuse_ui_widgets (widgets/extra.hpp): dialog button box and
# dialog default / reject keys, input dialog, tool box, LCD number, key sequence edit, MDI area, graphics scene / view
# (rubber-band selection), on-screen keyboard, and their class registration
fuse_ui_test(fuse_ui_widgets_extra test_widgets_extra.cpp)
foreach(g buttonbox dialog_keys input_dialog toolbox lcd keysequence mdi graphics vkeyboard classes)
    add_test(NAME fuse_ui_extra_${g} COMMAND fuse_ui_widgets_extra --gate=${g})
    set_tests_properties(fuse_ui_extra_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()

# Golden images of the ported widgets / options (tests/golden/port_<scene>.png; FUSE_UI_UPDATE_GOLDEN=1 rewrites).
# The goldens exist for the MINIMAL text profile only: the FULL profile (HarfBuzz + FreeType, fetched) could not be
# built where they were rendered, so with FULL the executable is built but no gate is registered.
add_executable(fuse_ui_port_golden test_port_golden.cpp)
target_link_libraries(fuse_ui_port_golden PRIVATE fuse_ui_widgets)
target_include_directories(fuse_ui_port_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
if(FUSE_UI_TEXT STREQUAL "FULL")
    target_compile_definitions(fuse_ui_port_golden PRIVATE FUSE_UI_TEST_TEXT_FULL=1)
else()
    foreach(g edit_options extra dock_split)
        add_test(NAME fuse_ui_port_golden_${g} COMMAND fuse_ui_port_golden --scene=${g})
        set_tests_properties(fuse_ui_port_golden_${g} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
endif()

# The ported widgets / options as .fuseui document classes and properties
if(TARGET fuse_ui_doc)
    fuse_ui_test(fuse_ui_port_doc test_port_doc.cpp)
    target_link_libraries(fuse_ui_port_doc PRIVATE fuse_ui_doc)
    set_tests_properties(fuse_ui_port_doc PROPERTIES LABELS "fuse_ui;gate")
endif()

# The ported widgets keep the zero-allocation rule in steady frames (as fuse_ui_zero_alloc_steady)
fuse_ui_test(fuse_ui_port_zero_alloc test_port_zero_alloc.cpp)
set_tests_properties(fuse_ui_port_zero_alloc PROPERTIES LABELS "fuse_ui;gate")
