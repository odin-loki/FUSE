# B6 Qt editor gates (label "gate;qt"): drive the real Qt 6 host (fuse_editor_qt) under a display
# (xvfb-run via fuse_run_with_display.sh when no DISPLAY is set). Without Qt every test skips (77).

set(_fuse_qt_gate_tests
    startup
    layout
    theme
    "font_1:font 1"
    "font_1_25:font 1.25"
    "font_1_5:font 1.5"
    "font_2:font 2"
    wasd
    context_menu
    "context_menu_dpr1_5:context_menu"
    fuse_api
    present_adopt
    live_present
    ui_frame
    idle_frames
)

if(FUSE_EDITOR_QT_AVAILABLE AND TARGET fuse_editor_qt)
    find_package(Qt6 QUIET COMPONENTS Test)
endif()

if(FUSE_EDITOR_QT_AVAILABLE AND TARGET fuse_editor_qt AND TARGET Qt6::Test)
    add_executable(fuse_editor_qt_gates test_editor_qt_gates.cpp)
    target_link_libraries(fuse_editor_qt_gates PRIVATE fuse_editor_qt fuse_core Qt6::Test)
    target_compile_definitions(fuse_editor_qt_gates PRIVATE FUSE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    fuse_apply_cxx23(fuse_editor_qt_gates)
    set(_fuse_qt_gate_available ON)
else()
    add_executable(fuse_editor_qt_gates qt_gates_unavailable.cpp)
    fuse_apply_cxx23(fuse_editor_qt_gates)
    set(_fuse_qt_gate_available OFF)
endif()

if(DEFINED FUSE_DISPLAY_TEST_WRAPPER AND EXISTS "${FUSE_DISPLAY_TEST_WRAPPER}")
    set(_fuse_qt_gate_launcher "${FUSE_DISPLAY_TEST_WRAPPER}")
else()
    set(_fuse_qt_gate_launcher "")
endif()

foreach(_entry IN LISTS _fuse_qt_gate_tests)
    if(_entry MATCHES "^([^:]+):(.*)$")
        set(_name "${CMAKE_MATCH_1}")
        separate_arguments(_args UNIX_COMMAND "${CMAKE_MATCH_2}")
    else()
        set(_name "${_entry}")
        set(_args "${_entry}")
    endif()
    set(_test fuse_editor_qt_${_name})
    add_test(NAME ${_test} COMMAND ${_fuse_qt_gate_launcher} $<TARGET_FILE:fuse_editor_qt_gates> ${_args})
    set(_env "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
    if(_name MATCHES "^font_(.*)$")
        string(REPLACE "_" "." _scale "${CMAKE_MATCH_1}")
        list(APPEND _env "QT_SCALE_FACTOR=${_scale}")
    elseif(_name STREQUAL "context_menu_dpr1_5")
        list(APPEND _env "QT_SCALE_FACTOR=1.5")
    endif()
    set(_labels "gate;qt")
    if(_name STREQUAL "live_present")
        # Needs a real X server (Xvfb via the display wrapper): the viewport presents to an X window.
        list(APPEND _labels xvfb)
    endif()
    set_tests_properties(${_test} PROPERTIES
        LABELS "${_labels}"
        SKIP_RETURN_CODE 77
        ENVIRONMENT "${_env}"
        TIMEOUT 600
        # Wall-clock / focus sensitive: never share the X server or the CPU with another Qt gate.
        RUN_SERIAL TRUE
    )
endforeach()

# Appendix A "Icons": FUSE app icon embedded in the Qt host and installed as the window icon.
if(_fuse_qt_gate_available)
    add_executable(fuse_editor_qt_icon_gate test_editor_qt_icon.cpp)
    target_link_libraries(fuse_editor_qt_icon_gate PRIVATE fuse_editor_qt)
    fuse_apply_cxx23(fuse_editor_qt_icon_gate)
    add_test(NAME fuse_editor_qt_icon COMMAND fuse_editor_qt_icon_gate)
else()
    add_test(NAME fuse_editor_qt_icon COMMAND fuse_editor_qt_gates icon)
endif()
set_tests_properties(fuse_editor_qt_icon PROPERTIES
    LABELS "gate;qt;lint"
    SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 120
)
