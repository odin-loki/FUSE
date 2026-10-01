# E20 Qt gates (label "gate;qt;e20", offscreen QPA): editable inspector + add / remove component,
# hierarchy drag-and-drop reparent, File menu (New / Open / Save / Save As / Recent, dirty prompt,
# title) and the New Project wizard. Without the Qt host every gate skips (77) like the others.
set(_fuse_e20_qt_gates inspector reparent file_menu wizard)
if(_fuse_qt_gate_available)
    add_executable(fuse_editor_qt_e20_gate test_editor_qt_inspector_files.cpp)
    target_link_libraries(fuse_editor_qt_e20_gate PRIVATE fuse_editor_qt fuse_core Qt6::Test)
    fuse_apply_cxx23(fuse_editor_qt_e20_gate)
    foreach(_e20_gate IN LISTS _fuse_e20_qt_gates)
        add_test(NAME fuse_editor_qt_e20_${_e20_gate} COMMAND fuse_editor_qt_e20_gate ${_e20_gate})
    endforeach()
else()
    foreach(_e20_gate IN LISTS _fuse_e20_qt_gates)
        add_test(NAME fuse_editor_qt_e20_${_e20_gate} COMMAND fuse_editor_qt_gates e20_${_e20_gate})
    endforeach()
endif()
foreach(_e20_gate IN LISTS _fuse_e20_qt_gates)
    set_tests_properties(fuse_editor_qt_e20_${_e20_gate} PROPERTIES
        LABELS "gate;qt;e20"
        SKIP_RETURN_CODE 77
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )
endforeach()
