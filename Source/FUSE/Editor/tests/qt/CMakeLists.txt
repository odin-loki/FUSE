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
    project_open
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

if(CMAKE_CROSSCOMPILING)
    # Cross build (e.g. MinGW + Wine): the host X display wrapper cannot run the target binary.
    set(_fuse_qt_gate_launcher ${CMAKE_CROSSCOMPILING_EMULATOR})
elseif(DEFINED FUSE_DISPLAY_TEST_WRAPPER AND EXISTS "${FUSE_DISPLAY_TEST_WRAPPER}")
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
    set(_env)
    if(NOT WIN32)
        # Linux CI / WSL: Mesa lavapipe; xcb under Xvfb (Wayland on WSLg aborts during Qt teardown).
        list(APPEND _env "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
        list(APPEND _env "QT_QPA_PLATFORM=xcb")
    endif()
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
# Full 10k-frame × up to 3 runs can exceed 30 min on Lavapipe/WSL (CI Release is faster).
set_tests_properties(fuse_editor_qt_idle_frames PROPERTIES TIMEOUT 3600)

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

# E15 (MP-B6-EDITOR-SCRIPT-PIE / UNI-U6-CON-1): Qt console dock (Lua REPL, level colours / filters,
# repeat counts, history) and the Play / Pause / Step / Resume / Stop transport, offscreen QPA.
if(_fuse_qt_gate_available)
    add_executable(fuse_editor_qt_console_transport_gate test_editor_qt_console_transport.cpp)
    target_link_libraries(fuse_editor_qt_console_transport_gate PRIVATE fuse_editor_qt fuse_core Qt6::Test)
    fuse_apply_cxx23(fuse_editor_qt_console_transport_gate)
    foreach(_e15_gate console transport)
        add_test(NAME fuse_editor_qt_e15_${_e15_gate} COMMAND fuse_editor_qt_console_transport_gate ${_e15_gate})
    endforeach()
else()
    foreach(_e15_gate console transport)
        add_test(NAME fuse_editor_qt_e15_${_e15_gate} COMMAND fuse_editor_qt_gates e15_${_e15_gate})
    endforeach()
endif()
foreach(_e15_gate console transport)
    set_tests_properties(fuse_editor_qt_e15_${_e15_gate} PROPERTIES
        LABELS "gate;qt;e15;u8"
        SKIP_RETURN_CODE 77
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )
endforeach()

# E20 (MP-B6-QT-INSPECTOR, UNI-U6-INSP-1, UNI-U6-FILE-1, MP-B6-QT-SCENE-FILES).
include(${CMAKE_CURRENT_SOURCE_DIR}/e20_qt_tests.cmake)

# GUI-1: desktop shell gates — docks, File / Edit toolbar, toolbar undo of an inspector edit,
# Play / Stop, opening a sample project (offscreen QPA).
set(_fuse_shell_qt_gates docks toolbar undo play assets profiler project)
if(_fuse_qt_gate_available)
    add_executable(fuse_editor_qt_shell_gate test_editor_qt_shell.cpp)
    target_link_libraries(fuse_editor_qt_shell_gate PRIVATE fuse_editor_qt fuse_core Qt6::Test)
    target_compile_definitions(fuse_editor_qt_shell_gate PRIVATE FUSE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    fuse_apply_cxx23(fuse_editor_qt_shell_gate)
    foreach(_shell_gate IN LISTS _fuse_shell_qt_gates)
        add_test(NAME fuse_editor_qt_shell_${_shell_gate} COMMAND fuse_editor_qt_shell_gate ${_shell_gate})
    endforeach()
else()
    foreach(_shell_gate IN LISTS _fuse_shell_qt_gates)
        add_test(NAME fuse_editor_qt_shell_${_shell_gate} COMMAND fuse_editor_qt_gates shell_${_shell_gate})
    endforeach()
endif()
foreach(_shell_gate IN LISTS _fuse_shell_qt_gates)
    set_tests_properties(fuse_editor_qt_shell_${_shell_gate} PROPERTIES
        LABELS "gate;qt;gui"
        SKIP_RETURN_CODE 77
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        TIMEOUT 180
    )
endforeach()

# CI / local: one build target for every Qt gate executable (icon, E15, E20, xvfb suite).
if(_fuse_qt_gate_available)
    add_custom_target(fuse_editor_qt_gates_all)
    add_dependencies(fuse_editor_qt_gates_all
        fuse_editor_qt_gates
        fuse_editor_qt_icon_gate
        fuse_editor_qt_console_transport_gate
        fuse_editor_qt_e20_gate
        fuse_editor_qt_shell_gate)
endif()
