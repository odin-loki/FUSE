# FUSE UI gates (plan §8) and extra tests. Every gate is a plain executable registered with ctest.
set(FUSE_UI_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(FUSE_UI_STB ${FUSE_UI_REPO_ROOT}/vendor/stb)

function(fuse_ui_test name src)
    add_executable(${name} ${src} ${ARGN})
    target_link_libraries(${name} PRIVATE fuse_ui_widgets)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS fuse_ui)
    if(FUSE_UI_TEXT STREQUAL "FULL")
        target_compile_definitions(${name} PRIVATE FUSE_UI_TEST_TEXT_FULL=1)
    endif()
endfunction()

fuse_ui_test(fuse_ui_layout_conformance test_layout_conformance.cpp)
fuse_ui_test(fuse_ui_style_cascade test_style_cascade.cpp)
fuse_ui_test(fuse_ui_event_routing test_event_routing.cpp)
fuse_ui_test(fuse_ui_focus_order test_focus_order.cpp)
fuse_ui_test(fuse_ui_cpu_raster_golden test_cpu_raster_golden.cpp)
set_tests_properties(fuse_ui_cpu_raster_golden PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
fuse_ui_test(fuse_ui_zero_alloc_steady test_zero_alloc_steady.cpp)
fuse_ui_test(fuse_ui_bench_10k test_bench_10k.cpp)
fuse_ui_test(fuse_ui_single_thread_parity test_single_thread_parity.cpp)
fuse_ui_test(fuse_ui_a11y_tree_dump test_a11y_tree_dump.cpp)
fuse_ui_test(fuse_ui_text_basics test_text_basics.cpp)
fuse_ui_test(fuse_text_edit_ops test_text_edit_ops.cpp)
fuse_ui_test(fuse_text_fallback test_text_fallback.cpp)
fuse_ui_test(fuse_ui_widget_interaction test_widget_interaction.cpp)
fuse_ui_test(fuse_ui_touch_scroll test_touch_scroll.cpp)

find_package(Threads REQUIRED)
target_link_libraries(fuse_ui_single_thread_parity PRIVATE Threads::Threads)
target_link_libraries(fuse_ui_event_routing PRIVATE Threads::Threads)

# Relight overlay::Ui (first client) vs ImmediateUi: compile the overlay's standalone UI sources directly.
set(FUSE_RELIGHT_OVERLAY ${CMAKE_CURRENT_SOURCE_DIR}/../../Relight/overlay)
if(EXISTS ${FUSE_RELIGHT_OVERLAY}/src/ui.cpp)
    fuse_ui_test(fuse_ui_relight_equivalence test_relight_equivalence.cpp
                 ${FUSE_RELIGHT_OVERLAY}/src/ui.cpp ${FUSE_RELIGHT_OVERLAY}/src/font5x7.cpp)
    target_include_directories(fuse_ui_relight_equivalence PRIVATE ${FUSE_RELIGHT_OVERLAY}/include)
endif()

# UI1 key / event v2: header-only, plus the v1 key tables from Core.
set(FUSE_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Core)
fuse_ui_test(fuse_ui_platform_keys_v2 test_platform_keys_v2.cpp ${FUSE_CORE_DIR}/src/platform/input.cpp
             ${FUSE_CORE_DIR}/src/platform/text_services.cpp)
target_include_directories(fuse_ui_platform_keys_v2 PRIVATE ${FUSE_CORE_DIR}/include)
# UI1 UI side: IME pre-edit, text-input session, clipboard, wheel / text through the bridge.
fuse_ui_test(fuse_ui_text_input test_text_input.cpp ${FUSE_CORE_DIR}/src/platform/input.cpp
             ${FUSE_CORE_DIR}/src/platform/text_services.cpp)
target_include_directories(fuse_ui_text_input PRIVATE ${FUSE_CORE_DIR}/include)
if(WIN32) # text_services.cpp uses IMM32 (ImmAssociateContextEx etc.)
    target_link_libraries(fuse_ui_platform_keys_v2 PRIVATE imm32)
    target_link_libraries(fuse_ui_text_input PRIVATE imm32)
endif()

# .fuseui documents and Lua bindings.
if(TARGET fuse_ui_doc)
    # ---- UI8 gates: cookers, binding bytecode, hot reload identity, inspector, cooked registration, fuzz ----
    set(FUSE_UI8_COOKED_DIR ${CMAKE_CURRENT_BINARY_DIR}/cooked)
    set(FUSE_UI8_REG_CPP ${FUSE_UI8_COOKED_DIR}/cooked_registration.cpp)
    set(FUSE_UI8_CAN_COOK ON)
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        set(FUSE_UI8_CAN_COOK OFF)
    endif()
    if(FUSE_UI8_CAN_COOK)
        add_custom_command(
            OUTPUT ${FUSE_UI8_REG_CPP} ${FUSE_UI8_COOKED_DIR}/settings.fuseuic ${FUSE_UI8_COOKED_DIR}/theme.fusestylec
            COMMAND ${CMAKE_COMMAND} -E make_directory ${FUSE_UI8_COOKED_DIR}
            COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:fuse_ui_cook> --out=${FUSE_UI8_COOKED_DIR}
                    --registration=${FUSE_UI8_REG_CPP} ${CMAKE_CURRENT_SOURCE_DIR}/data/settings.fuseui
                    ${CMAKE_CURRENT_SOURCE_DIR}/data/theme.fusestyle
            DEPENDS fuse_ui_cook ${CMAKE_CURRENT_SOURCE_DIR}/data/settings.fuseui ${CMAKE_CURRENT_SOURCE_DIR}/data/theme.fusestyle
            COMMENT "fuse_ui_cook: cooking test documents + registration list"
            VERBATIM)
    endif()
    foreach(g markup_roundtrip binding_bytecode style_cooked hot_reload_identity inspector cooked_registration markup_fuzz)
        if(g STREQUAL "cooked_registration" AND NOT FUSE_UI8_CAN_COOK)
            continue()
        endif()
        set(t fuse_ui_${g})
        if(g STREQUAL "cooked_registration")
            fuse_ui_test(${t} test_ui8.cpp ${FUSE_UI8_REG_CPP})
            target_compile_definitions(${t} PRIVATE FUSE_UI8_HAVE_COOKED_REGISTRATION=1 FUSE_UI_COOKED_DIR="${FUSE_UI8_COOKED_DIR}")
        else()
            fuse_ui_test(${t} test_ui8.cpp)
        endif()
        target_link_libraries(${t} PRIVATE fuse_ui_doc)
        target_compile_definitions(${t} PRIVATE FUSE_UI8_GATE="${g}" FUSE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
        set_tests_properties(${t} PROPERTIES LABELS "fuse_ui;gate")
    endforeach()

    fuse_ui_test(fuse_ui_doc_load test_doc.cpp)
    target_link_libraries(fuse_ui_doc_load PRIVATE fuse_ui_doc)
    target_compile_definitions(fuse_ui_doc_load PRIVATE FUSE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
endif()
if(TARGET fuse_ui_script)
    fuse_ui_test(fuse_ui_lua_bindings test_lua.cpp)
    target_link_libraries(fuse_ui_lua_bindings PRIVATE fuse_ui_script)
    target_compile_definitions(fuse_ui_lua_bindings PRIVATE FUSE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    # UI8 gate: the whole `ui` table (coverage of every function, errors, signals, handler lifetime, steady frames)
    fuse_ui_test(fuse_ui_script_bind test_script_bind.cpp)
    target_link_libraries(fuse_ui_script_bind PRIVATE fuse_ui_script)
    target_compile_definitions(fuse_ui_script_bind PRIVATE FUSE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    set_tests_properties(fuse_ui_script_bind PROPERTIES LABELS "fuse_ui;gate")
endif()

add_executable(fuse_ui_sample sample_main.cpp)
target_link_libraries(fuse_ui_sample PRIVATE fuse_ui_widgets)
target_include_directories(fuse_ui_sample PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})

# UI4 gate: GPU draw-list backend vs the CPU reference (lavapipe / any Vulkan device; 77 = skipped, no device).
if(TARGET fuse_ui_vk)
    fuse_ui_test(fuse_ui_vk_parity test_vk_parity.cpp)
    target_link_libraries(fuse_ui_vk_parity PRIVATE fuse_ui_vk ${FUSE_UI_VK_LINK})
    set_tests_properties(fuse_ui_vk_parity PROPERTIES SKIP_RETURN_CODE 77 LABELS "ui;gpu")
endif()

fuse_ui_test(fuse_ui_text_full test_text_full.cpp)
fuse_ui_test(fuse_ui_tier_b test_tier_b.cpp)
fuse_ui_test(fuse_ui_tier_b2 test_tier_b2.cpp)
fuse_ui_test(fuse_ui_backdrop_blur test_backdrop_blur.cpp)
# HiDPI: UiContext::setRenderScale (logical layout / input, device-pixel draw list, glyphs rasterised at the scaled size)
fuse_ui_test(fuse_ui_render_scale test_render_scale.cpp)
set_tests_properties(fuse_ui_render_scale PROPERTIES LABELS "fuse_ui;gate")
# UI6 gates: dock layout save / restore, 1M-row model / view, Tier C (proxy, delegates, grid), Tier D (rich widgets)
foreach(g dock_layout_roundtrip modelview_1m_rows tier_c tier_d)
    fuse_ui_test(fuse_ui_${g} test_ui6.cpp)
    target_compile_definitions(fuse_ui_${g} PRIVATE FUSE_UI6_GATE="${g}")
    set_tests_properties(fuse_ui_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()

# fuse_ui_a11y: platform accessibility bridge (+ AccessKit when FUSE_UI_ACCESSKIT_DIR is set)
if(TARGET fuse_ui_a11y)
    fuse_ui_test(fuse_ui_a11y_bridge test_a11y_bridge.cpp)
    target_link_libraries(fuse_ui_a11y_bridge PRIVATE fuse_ui_a11y)
endif()

# fuse_ui_sql: SqliteModel over vendor/sqlite
if(FUSE_UI_HAVE_SQL)
    fuse_ui_test(fuse_ui_sql_model test_sql_model.cpp)
    target_link_libraries(fuse_ui_sql_model PRIVATE fuse_ui_sql)
endif()

# Theora video (UI10 media)
if(TARGET fuse_ui_video)
    fuse_ui_test(fuse_ui_theora_video test_theora_video.cpp)
    target_link_libraries(fuse_ui_theora_video PRIVATE fuse_ui_video fuse_ui_theoraenc)
endif()

# UI8 designer prototype
if(TARGET fuse_ui_designer_lib)
    fuse_ui_test(fuse_ui_designer_roundtrip test_designer.cpp)
    target_link_libraries(fuse_ui_designer_roundtrip PRIVATE fuse_ui_designer_lib)
endif()

# UI10 platform feel: kinetic scroll physics, haptics hook
fuse_ui_test(fuse_ui_platform_feel test_platform_feel.cpp)

# UI6 Tier D (2): one binary, one ctest gate per widget (fuse_ui_widget_<name>)
fuse_ui_test(fuse_ui_tier_b3 test_tier_b3.cpp)
foreach(g groupbox stack toolbar_statusbar mainwindow contextmenu messagebox wizard popover_toast header textedit richtext
          catalogue_classes)
    add_test(NAME fuse_ui_widget_${g} COMMAND fuse_ui_tier_b3 --gate=${g})
    set_tests_properties(fuse_ui_widget_${g} PROPERTIES LABELS fuse_ui)
endforeach()
# The last §5 pieces: flame-graph chart, tabbed / floating docks and their layout round trip
fuse_ui_test(fuse_ui_tier_b4 test_tier_b4.cpp)
foreach(g chart_flame dock_tabs dock_float dock_layout_tabs_float)
    add_test(NAME fuse_ui_widget_${g} COMMAND fuse_ui_tier_b4 --gate=${g})
    set_tests_properties(fuse_ui_widget_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()
# Golden images of the catalogue widgets above (tests/golden/widgets_<scene>[_full].png, FUSE_UI_UPDATE_GOLDEN=1
# rewrites): one binary, one ctest gate per scene. The hud scene (minimap, ammo counter) is added with fuse_ui_game.
add_executable(fuse_ui_widget_golden test_widget_golden.cpp)
target_link_libraries(fuse_ui_widget_golden PRIVATE fuse_ui_widgets)
target_include_directories(fuse_ui_widget_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
if(FUSE_UI_TEXT STREQUAL "FULL")
    target_compile_definitions(fuse_ui_widget_golden PRIVATE FUSE_UI_TEST_TEXT_FULL=1)
endif()
foreach(g containers text dialogs wizard mainwindow)
    add_test(NAME fuse_ui_widget_golden_${g} COMMAND fuse_ui_widget_golden --scene=${g})
    set_tests_properties(fuse_ui_widget_golden_${g} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
fuse_ui_test(fuse_ui_tier_d2 test_tier_d2.cpp)
foreach(g font_picker file_dialog node_graph curve_timeline viewport code_edit datetime_edit video)
    add_test(NAME fuse_ui_widget_${g} COMMAND fuse_ui_tier_d2 --gate=${g})
    set_tests_properties(fuse_ui_widget_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()

# UI5 gates: fuse_app on-demand loop (virtual clock, headless presenter)
if(TARGET fuse_app)
    foreach(g idle_zero_frames multiwindow hello_startup settings)
        add_executable(fuse_app_${g} test_app.cpp)
        target_link_libraries(fuse_app_${g} PRIVATE fuse_app)
        target_include_directories(fuse_app_${g} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(fuse_app_${g} PRIVATE FUSE_APP_GATE="${g}")
        add_test(NAME fuse_app_${g} COMMAND fuse_app_${g})
        set_tests_properties(fuse_app_${g} PROPERTIES LABELS "fuse_ui;gate")
    endforeach()
    add_executable(fuse_app_render_scale test_app_render_scale.cpp)
    target_link_libraries(fuse_app_render_scale PRIVATE fuse_app)
    add_test(NAME fuse_app_render_scale COMMAND fuse_app_render_scale --out=${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(fuse_app_render_scale PROPERTIES LABELS "fuse_ui;gate")
    if(TARGET fuse_app_native)
        add_executable(fuse_app_native_window test_app_native.cpp)
        target_link_libraries(fuse_app_native_window PRIVATE fuse_app_native)
        find_program(FUSE_UI_XVFB_RUN xvfb-run)
        if(FUSE_UI_XVFB_RUN AND NOT WIN32)
            add_test(NAME fuse_app_native_window COMMAND ${FUSE_UI_XVFB_RUN} -a -s "-screen 0 1280x800x24"
                     $<TARGET_FILE:fuse_app_native_window> --out=${CMAKE_CURRENT_BINARY_DIR})
        else()
            add_test(NAME fuse_app_native_window COMMAND fuse_app_native_window --out=${CMAKE_CURRENT_BINARY_DIR})
        endif()
        set_tests_properties(fuse_app_native_window PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" LABELS "fuse_ui;gate;vulkan")
        add_executable(fuse_app_os_popups test_app_os_popups.cpp)
        target_link_libraries(fuse_app_os_popups PRIVATE fuse_app_native)
        if(FUSE_UI_XVFB_RUN AND NOT WIN32)
            add_test(NAME fuse_app_os_popups COMMAND ${FUSE_UI_XVFB_RUN} -a -s "-screen 0 1280x800x24"
                     $<TARGET_FILE:fuse_app_os_popups> --out=${CMAKE_CURRENT_BINARY_DIR})
        else()
            add_test(NAME fuse_app_os_popups COMMAND fuse_app_os_popups --out=${CMAKE_CURRENT_BINARY_DIR})
        endif()
        set_tests_properties(fuse_app_os_popups PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" LABELS "fuse_ui;gate;vulkan")
    endif()
    if(TARGET hello_app)
        add_test(NAME fuse_app_hello_sample COMMAND hello_app --screenshot=${CMAKE_CURRENT_BINARY_DIR}/hello_app.png)
        add_test(NAME fuse_app_notes_sample COMMAND notes_app --settings=${CMAKE_CURRENT_BINARY_DIR}/notes_settings.ini
                 --screenshot=${CMAKE_CURRENT_BINARY_DIR}/notes_app.png)
        set_tests_properties(fuse_app_hello_sample fuse_app_notes_sample PROPERTIES LABELS fuse_ui)
    endif()
endif()

# ---- UI7 game UI gates ----
if(TARGET fuse_ui_game)
    foreach(pair "hud_golden=fuse_ui_game_hud_golden" "gamepad_nav=fuse_ui_gamepad_nav"
                 "world_panel_raycast=fuse_ui_world_panel_raycast" "input_arbitration=fuse_ui_input_arbitration"
                 "pause_time_domain=fuse_ui_pause_time_domain")
        string(REPLACE "=" ";" kv ${pair})
        list(GET kv 0 g)
        list(GET kv 1 t)
        fuse_ui_test(${t} test_game.cpp)
        target_link_libraries(${t} PRIVATE fuse_ui_game)
        target_compile_definitions(${t} PRIVATE FUSE_UI_GAME_GATE="${g}")
        set_tests_properties(${t} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
    foreach(pair "inventory_dnd=fuse_ui_inventory_dnd" "touch_controls=fuse_ui_touch_controls"
                 "loading_screen=fuse_ui_loading_screen" "rich_dialogue=fuse_ui_rich_dialogue"
                 "sound_theme=fuse_ui_sound_theme")
        string(REPLACE "=" ";" kv ${pair})
        list(GET kv 0 g)
        list(GET kv 1 t)
        fuse_ui_test(${t} test_game2.cpp)
        target_link_libraries(${t} PRIVATE fuse_ui_game)
        target_compile_definitions(${t} PRIVATE FUSE_UI_GAME_GATE="${g}")
        set_tests_properties(${t} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
    # HUD widgets added with the rest of the catalogue: one ctest per widget (fuse_ui_widget_<name>)
    foreach(g minimap ammo_counter)
        fuse_ui_test(fuse_ui_widget_${g} test_game3.cpp)
        target_link_libraries(fuse_ui_widget_${g} PRIVATE fuse_ui_game)
        target_compile_definitions(fuse_ui_widget_${g} PRIVATE FUSE_UI_GAME_GATE="${g}")
        set_tests_properties(fuse_ui_widget_${g} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
    target_link_libraries(fuse_ui_widget_golden PRIVATE fuse_ui_game)
    target_compile_definitions(fuse_ui_widget_golden PRIVATE FUSE_UI_GOLDEN_HAVE_GAME=1)
    add_test(NAME fuse_ui_widget_golden_hud COMMAND fuse_ui_widget_golden --scene=hud)
    set_tests_properties(fuse_ui_widget_golden_hud PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # localisation: plural rules, messages, number / date formatting, StringTable tr, fuse_ui_extract_strings
    if(TARGET fuse_ui_extract_strings AND NOT CMAKE_CROSSCOMPILING)
        fuse_ui_test(fuse_ui_l10n test_l10n.cpp)
        target_link_libraries(fuse_ui_l10n PRIVATE fuse_ui_game fuse_ui_doc)
        add_dependencies(fuse_ui_l10n fuse_ui_extract_strings)
        target_compile_definitions(fuse_ui_l10n PRIVATE FUSE_UI_EXTRACT_EXE="$<TARGET_FILE:fuse_ui_extract_strings>"
                                   FUSE_UI_TEST_OUT="${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(fuse_ui_l10n PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    # plan §6 HUD budget: ~300-node HUD animated as in play, UI CPU per frame median <= 0.5 ms (the scene is shared
    # with Game/samples/hud_bench.cpp, the Android bench app that is not built yet: docs/ui/archive/PORT_magical_bassi.md)
    # Registered since StyleEngine's style hash covers placement and all colour channels: before that every style a HUD
    # animates (anchored / absolute offsets, fades) landed in one probe chain and main measured 0.57-0.76 ms; now
    # ~0.10 ms on the same machine (docs/ui/BASELINES.md).
    add_executable(fuse_ui_bench_hud test_bench_hud.cpp)
    target_link_libraries(fuse_ui_bench_hud PRIVATE fuse_ui_game)
    target_include_directories(fuse_ui_bench_hud PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB} ${CMAKE_CURRENT_SOURCE_DIR}/../Game/samples)
    add_test(NAME fuse_ui_bench_hud COMMAND fuse_ui_bench_hud)
    set_tests_properties(fuse_ui_bench_hud PROPERTIES LABELS "fuse_ui;gate;perf" RUN_SERIAL TRUE) # timing budget
    if(TARGET fuse_ui_game_ecs)
        fuse_ui_test(fuse_ui_ecs_surface test_game_bridges.cpp)
        target_link_libraries(fuse_ui_ecs_surface PRIVATE fuse_ui_game_ecs)
        target_compile_definitions(fuse_ui_ecs_surface PRIVATE FUSE_UI_GATE_ECS=1)
        set_tests_properties(fuse_ui_ecs_surface PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    if(TARGET fuse_ui_game_audio)
        fuse_ui_test(fuse_ui_ui_audio test_game_bridges.cpp)
        target_link_libraries(fuse_ui_ui_audio PRIVATE fuse_ui_game_audio)
        target_compile_definitions(fuse_ui_ui_audio PRIVATE FUSE_UI_GATE_AUDIO=1)
        set_tests_properties(fuse_ui_ui_audio PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    if(TARGET hud_demo)
        add_test(NAME fuse_ui_game_hud_demo COMMAND hud_demo --screenshot=${CMAKE_CURRENT_BINARY_DIR}/hud_demo.png)
        add_test(NAME fuse_ui_game_world_panel_demo COMMAND world_panel_demo --screenshot=${CMAKE_CURRENT_BINARY_DIR}/world_panel_demo.png)
        set_tests_properties(fuse_ui_game_hud_demo fuse_ui_game_world_panel_demo PROPERTIES LABELS fuse_ui)
    endif()
endif()

# ---- text stack: font families / synthetic styles / installed fonts / decorations (+ golden), i18n helpers ----
fuse_ui_test(fuse_text_font_styles test_text_styles.cpp)
target_compile_definitions(fuse_text_font_styles PRIVATE FUSE_UI_TEST_FONT_DIR="${FUSE_UI_REPO_ROOT}/vendor/fonts")
set_tests_properties(fuse_text_font_styles PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
fuse_ui_test(fuse_text_i18n test_i18n.cpp)
set_tests_properties(fuse_text_i18n PROPERTIES LABELS "fuse_ui;gate")
if(TARGET fuse_ui_game)
    target_link_libraries(fuse_text_i18n PRIVATE fuse_ui_game)
    target_compile_definitions(fuse_text_i18n PRIVATE FUSE_UI_I18N_HAVE_GAME=1)
endif()

# Platform-service gates (plan 4.13): cursors, menus, HiDPI maths, system theme, window state, file dialogs (headless)
if(TARGET fuse_app)
    foreach(g cursor_resolution menu_description dpi_scale theme_restyle window_state file_dialog_fallback)
        add_executable(fuse_app_${g} test_app_services.cpp)
        target_link_libraries(fuse_app_${g} PRIVATE fuse_app)
        target_include_directories(fuse_app_${g} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(fuse_app_${g} PRIVATE FUSE_APP_GATE="${g}" FUSE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
        add_test(NAME fuse_app_${g} COMMAND fuse_app_${g})
        set_tests_properties(fuse_app_${g} PROPERTIES LABELS "fuse_ui;gate")
    endforeach()
endif()

# Native platform services on real OS windows (X11: under Xvfb when xvfb-run exists; skipped without a display):
# HiDPI scale, cursors, clipboard between two connections, window management, displays, file-dialog / shell helpers.
if(TARGET fuse_app_native AND UNIX AND NOT APPLE AND NOT ANDROID)
    add_executable(fuse_app_native_services test_app_native_services.cpp)
    target_link_libraries(fuse_app_native_services PRIVATE fuse_app_native)
    find_package(Threads REQUIRED)
    target_link_libraries(fuse_app_native_services PRIVATE Threads::Threads)
    find_program(FUSE_UI_XVFB_RUN xvfb-run)
    if(FUSE_UI_XVFB_RUN)
        add_test(NAME fuse_app_native_services COMMAND ${FUSE_UI_XVFB_RUN} -a -s "-screen 0 1280x800x24"
                 $<TARGET_FILE:fuse_app_native_services> --out=${CMAKE_CURRENT_BINARY_DIR})
    else()
        add_test(NAME fuse_app_native_services COMMAND fuse_app_native_services --out=${CMAKE_CURRENT_BINARY_DIR})
    endif()
    set_tests_properties(fuse_app_native_services PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300 LABELS "fuse_ui;gate")
endif()

# Core interaction (plan §4.5 - §4.7, §4.10): drag and drop, gestures / multi-touch / double click, actions and
# shortcuts, style transitions and property animation, direction: rtl.
foreach(g drag_drop gestures actions style_motion rtl_layout)
    fuse_ui_test(fuse_ui_${g} test_${g}.cpp)
    set_tests_properties(fuse_ui_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()
if(TARGET fuse_ui_doc AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS") # writes a temp file
    fuse_ui_test(fuse_ui_hot_reload_file test_hot_reload_file.cpp)
    target_link_libraries(fuse_ui_hot_reload_file PRIVATE fuse_ui_doc)
    set_tests_properties(fuse_ui_hot_reload_file PROPERTIES LABELS "fuse_ui;gate")
    # style sheet hot reload (StyleSheetFileWatcher: replaced in place by handle)
    fuse_ui_test(fuse_ui_hot_reload_style test_hot_reload.cpp)
    target_link_libraries(fuse_ui_hot_reload_style PRIVATE fuse_ui_doc)
    set_tests_properties(fuse_ui_hot_reload_style PROPERTIES LABELS "fuse_ui;gate")
endif()
# Golden of the interaction visuals (action menu with shortcut texts; list drag ghost + insertion line). The image was
# rendered with the MINIMAL text profile; there is no FULL-profile golden, so the gate exists for MINIMAL only.
if(NOT FUSE_UI_TEXT STREQUAL "FULL")
    fuse_ui_test(fuse_ui_interaction_golden test_interaction_golden.cpp)
    set_tests_properties(fuse_ui_interaction_golden PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Model / view (plan §4.11; fuse/ui/widgets/modelview.hpp): one binary, one ctest gate per --gate (fuse_ui_mv_<name>)
# and one golden per --scene (tests/golden/modelview_<scene>[_full].png, FUSE_UI_UPDATE_GOLDEN=1 rewrites).
find_package(Threads)
add_executable(fuse_ui_modelview test_modelview.cpp)
target_link_libraries(fuse_ui_modelview PRIVATE fuse_ui_widgets)
if(TARGET Threads::Threads)
    target_link_libraries(fuse_ui_modelview PRIVATE Threads::Threads) # the async gate delivers a page from a worker thread
endif()
target_include_directories(fuse_ui_modelview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
if(FUSE_UI_TEXT STREQUAL "FULL")
    target_compile_definitions(fuse_ui_modelview PRIVATE FUSE_UI_TEST_TEXT_FULL=1)
endif()
foreach(g notify selection selection_1m tree tree_1m editing view proxy async dir_model)
    add_test(NAME fuse_ui_mv_${g} COMMAND fuse_ui_modelview --gate=${g})
    set_tests_properties(fuse_ui_mv_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()
set_tests_properties(fuse_ui_mv_dir_model PROPERTIES SKIP_RETURN_CODE 77) # no writable temp directory
foreach(g tree table_edit)
    add_test(NAME fuse_ui_mv_golden_${g} COMMAND fuse_ui_modelview --scene=${g})
    set_tests_properties(fuse_ui_mv_golden_${g} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
# ---- Drawing: vector paths, Painter, SVG (plan §4.9; parity rows "Qt GUI - painting", "Qt SVG") ----
# paths (fill / stroke / gradients / clips / 9-slice / render scale), the Painter, SVG parse + render + widgets,
# steady-state allocations and the icon rasterisation budget
foreach(g paths painter svg vector_steady)
    fuse_ui_test(fuse_ui_${g} test_${g}.cpp)
    set_tests_properties(fuse_ui_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()
# golden images: a paths / gradients sheet and the built-in icons at several sizes and render scales
add_executable(fuse_ui_vector_golden test_vector_golden.cpp)
target_link_libraries(fuse_ui_vector_golden PRIVATE fuse_ui_widgets)
target_include_directories(fuse_ui_vector_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
foreach(g paths icons)
    add_test(NAME fuse_ui_vector_golden_${g} COMMAND fuse_ui_vector_golden --scene=${g})
    set_tests_properties(fuse_ui_vector_golden_${g} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
# GPU packer: vector ops become kernel Image commands over CPU pre-rasterised texels (checked with a CPU model of
# the kernel's Image op; the kernel itself is covered by fuse_ui_vk_parity where a Vulkan device exists)
fuse_ui_test(fuse_ui_gpu_pack_vector test_gpu_pack_vector.cpp)
target_link_libraries(fuse_ui_gpu_pack_vector PRIVATE fuse_ui_render)
set_tests_properties(fuse_ui_gpu_pack_vector PROPERTIES LABELS "fuse_ui;gate")

# Dock drag and drop, catalogue coverage / extra goldens, accessibility families, sample and packaging gates
include(${CMAKE_CURRENT_SOURCE_DIR}/finishing.cmake)

# ---- gates from the gui-framework branch (docs/ui/archive/PORT_gui_framework.md) ----
# Key-code table of the branch's Cocoa backend (App/src/cocoa_map.hpp, a plain header; the backend itself is parked)
add_executable(fuse_app_cocoa_map test_cocoa_map.cpp)
target_link_libraries(fuse_app_cocoa_map PRIVATE fuse_ui_core)
add_test(NAME fuse_app_cocoa_map COMMAND fuse_app_cocoa_map)
set_tests_properties(fuse_app_cocoa_map PROPERTIES LABELS "fuse_ui;gate")

if(TARGET fuse_ui_os)
    # the fuse::ui::os seam on the configured backend (std by default)
    add_executable(fuse_ui_os_seam test_os_seam.cpp)
    target_link_libraries(fuse_ui_os_seam PRIVATE fuse_ui_os)
    target_include_directories(fuse_ui_os_seam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
    target_link_libraries(fuse_ui_os_seam PRIVATE fuse_ui_widgets) # test_util.hpp (font helpers)
    add_test(NAME fuse_ui_os_seam COMMAND fuse_ui_os_seam)
    set_tests_properties(fuse_ui_os_seam PROPERTIES LABELS "fuse_ui;gate")
endif()

# fuse_ui_extras: one ctest per gate of the modules main builds. The gates of the parked modules (gesture i18n
# containers dialogs lcd_keyseq mdi completer graphics widget_pdf richtext vkeyboard svg charts media) are in
# test_extras.cpp behind FUSE_UI_EXTRAS_ALL and are not registered.
if(TARGET fuse_ui_extras)
    add_executable(fuse_ui_extras_tests test_extras.cpp)
    find_package(Threads REQUIRED) # the tray gate serves D-Bus from a std::thread
    target_link_libraries(fuse_ui_extras_tests PRIVATE fuse_ui_extras Threads::Threads)
    target_include_directories(fuse_ui_extras_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
    target_compile_definitions(fuse_ui_extras_tests PRIVATE FUSE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    foreach(g undo desktop pdf tray declarative dataviz3d dataviz3d_bsp)
        add_test(NAME fuse_ui_extras_${g} COMMAND fuse_ui_extras_tests --gate=${g})
        set_tests_properties(fuse_ui_extras_${g} PROPERTIES LABELS "fuse_ui;gate")
    endforeach()
    # charts library (fuse::ui::charts) and the 3D data visualisation behind it
    add_executable(fuse_ui_charts_tests test_charts.cpp)
    target_link_libraries(fuse_ui_charts_tests PRIVATE fuse_ui_extras)
    target_include_directories(fuse_ui_charts_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
    foreach(g series axes decimation ring hit widget model export bench demo 3d 3d_large demo3d mesh3d mesh3d_large beyond transition demo_beyond)
        add_test(NAME fuse_ui_charts_${g} COMMAND fuse_ui_charts_tests --gate=${g})
        set_tests_properties(fuse_ui_charts_${g} PROPERTIES LABELS "fuse_ui;gate")
    endforeach()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") # partial designated initialisers in the branch's tests and in stb
        target_compile_options(fuse_ui_extras_tests PRIVATE -Wno-missing-field-initializers)
        target_compile_options(fuse_ui_charts_tests PRIVATE -Wno-missing-field-initializers)
    endif()
    # system web engine (WebKitGTK, loaded at run time): needs a display, so Linux / BSD runs it on its own Xvfb
    find_program(FUSE_XVFB_RUN Xvfb)
    if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT CMAKE_CROSSCOMPILING AND FUSE_XVFB_RUN)
        add_test(NAME fuse_ui_extras_webview
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_isolated_x.sh -- $<TARGET_FILE:fuse_ui_extras_tests> --gate=webview)
        set_tests_properties(fuse_ui_extras_webview PROPERTIES LABELS "fuse_ui;gate" TIMEOUT 120
            ENVIRONMENT "WEBKIT_DISABLE_COMPOSITING_MODE=1;LIBGL_ALWAYS_SOFTWARE=1")
    endif()
    # Win32 tray + HMENU global menu bar (the branch ran it under Wine; not run on main)
    if(WIN32)
        add_executable(fuse_ui_tray_win32 test_tray_win32.cpp)
        target_link_libraries(fuse_ui_tray_win32 PRIVATE fuse_ui_extras)
        add_test(NAME fuse_ui_tray_win32 COMMAND fuse_ui_tray_win32)
        set_tests_properties(fuse_ui_tray_win32 PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS "fuse_ui;gate")
    endif()
endif()

# Wall-clock budget gates also carry the label `perf`: their thresholds assume a quiet machine, so CI on shared or
# nested VMs (FreeBSD VM, macOS runners) runs `ctest -LE "vulkan|perf"`. That also leaves out what else these gates
# check (steady-state allocation counts, row counts); the Linux gates run them all.
foreach(t fuse_ui_bench_10k fuse_ui_modelview_1m_rows fuse_ui_mv_selection_1m fuse_ui_mv_tree_1m fuse_ui_vector_steady
          fuse_app_hello_startup)
    if(TEST ${t})
        get_test_property(${t} LABELS _fuse_ui_labels)
        set_tests_properties(${t} PROPERTIES LABELS "${_fuse_ui_labels};perf")
    endif()
endforeach()

# fuse_app platform layer, the parts that need no display (docs/ui/archive/PORT_magical_bassi.md 3.5 / 3.7,
# PORT_gui_framework.md 3.1 / 3.5): saved state, client-side window chrome, the window-less dialog helpers, the PNG
# clipboard codec, frame pacing and the tool-window surfaces of floating docks. The OS side (XDND, clipboard formats,
# tool windows, chrome drags, saved-state restore on real windows) is in fuse_app_native_services above.
if(TARGET fuse_app)
    add_executable(fuse_app_platform test_app_platform.cpp)
    target_link_libraries(fuse_app_platform PRIVATE fuse_app)
    target_include_directories(fuse_app_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    foreach(g saved_state window_chrome dialog_helpers clipboard_png frame_pacing tool_surfaces)
        add_test(NAME fuse_app_${g} COMMAND fuse_app_platform --gate=${g} --out=${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(fuse_app_${g} PROPERTIES LABELS "fuse_ui;gate")
    endforeach()
endif()

# Native desktop notifications (desktop::Notifier, TrayIcon::showMessage; PORT_magical_bassi.md 3.8): a private
# dbus-daemon with an in-process org.freedesktop.Notifications service. Passes with a "skipped" line without libdbus.
if(TARGET fuse_ui_extras_tests)
    add_test(NAME fuse_ui_extras_notify COMMAND fuse_ui_extras_tests --gate=notify)
    set_tests_properties(fuse_ui_extras_notify PROPERTIES LABELS "fuse_ui;gate" TIMEOUT 120)
endif()

# ---- Wayland backend of fuse_app_native (Linux / BSD; port list mb 3.6 + gf 3.2) ----
# fuse_app_wayland: protocol tables / keymaps / backend selection without a compositor, then real windows under a
# headless weston the gate starts itself (found in PATH or $FUSE_TEST_WESTON at run time; exit 77 without one or
# without libwayland-client). fuse_app_wayland_seat: the seat (pointer, keyboard, touch, scroll, clipboard, drops,
# IME, fractional scale) against a small compositor the test runs in-process on libwayland-server (exit 77 without it).
if(TARGET fuse_app_native AND UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(Threads REQUIRED)
    foreach(g fuse_app_wayland fuse_app_wayland_seat)
        if(g STREQUAL "fuse_app_wayland")
            add_executable(${g} test_app_wayland.cpp)
        else()
            add_executable(${g} test_app_wayland_seat.cpp)
        endif()
        target_link_libraries(${g} PRIVATE fuse_app_native fuse_ui_widgets Threads::Threads ${CMAKE_DL_LIBS})
        target_include_directories(${g} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME ${g} COMMAND ${g})
        set_tests_properties(${g} PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300 LABELS "fuse_ui;gate"
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
    endforeach()
    # Every other gate keeps testing what it was written for: on a developer's Wayland session NativePlatform would
    # pick Wayland ($WAYLAND_DISPLAY), so they pin X11. Deferred to the end of this directory so that gates added
    # below this block are covered too.
    function(fuse_ui_pin_x11_gates)
        get_property(_all DIRECTORY PROPERTY TESTS)
        foreach(t IN LISTS _all)
            if(NOT t MATCHES "^fuse_app_wayland")
                set_property(TEST ${t} APPEND PROPERTY ENVIRONMENT "FUSE_UI_BACKEND=x11")
            endif()
        endforeach()
    endfunction()
    cmake_language(DEFER CALL fuse_ui_pin_x11_gates)
endif()

# ---- themes: light / high contrast token sets, sheets by handle, contrast and focus-ring gates -----------------------
add_executable(fuse_ui_themes test_themes.cpp)
target_link_libraries(fuse_ui_themes PRIVATE fuse_ui_widgets)
target_include_directories(fuse_ui_themes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
foreach(g sheet_handles token_only contrast focus_ring)
    add_test(NAME fuse_ui_theme_${g} COMMAND fuse_ui_themes --gate=${g})
    set_tests_properties(fuse_ui_theme_${g} PROPERTIES LABELS "fuse_ui;gate")
endforeach()
# Golden images of the widget gallery scenes (test_widget_golden.cpp) in the light and high-contrast themes, and of the
# controls / editors scenes in all three. MINIMAL text profile only: the FULL-profile twins need the fetched text stack
# to be rendered and are not checked in.
if(NOT FUSE_UI_TEXT STREQUAL "FULL")
    foreach(g controls editors)
        add_test(NAME fuse_ui_widget_golden_${g} COMMAND fuse_ui_widget_golden --scene=${g})
        set_tests_properties(fuse_ui_widget_golden_${g} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
    set(_fuse_ui_theme_scenes containers text dialogs wizard mainwindow spinmenu controls editors)
    if(TARGET fuse_ui_game)
        list(APPEND _fuse_ui_theme_scenes hud)
    endif()
    foreach(t light contrast)
        foreach(g ${_fuse_ui_theme_scenes})
            add_test(NAME fuse_ui_theme_golden_${t}_${g} COMMAND fuse_ui_widget_golden --scene=${g} --theme=${t})
            set_tests_properties(fuse_ui_theme_golden_${t}_${g} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        endforeach()
    endforeach()
endif()
if(TARGET fuse_app)
    add_executable(fuse_app_theme_sheets test_theme_app.cpp)
    target_link_libraries(fuse_app_theme_sheets PRIVATE fuse_app)
    target_include_directories(fuse_app_theme_sheets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME fuse_app_theme_sheets COMMAND fuse_app_theme_sheets)
    set_tests_properties(fuse_app_theme_sheets PROPERTIES LABELS "fuse_ui;gate")
endif()

# ---- Extras rich text editor on main's text stack (Extras/src/richtext.cpp) ---------------------------------------
if(TARGET fuse_ui_extras)
    add_executable(fuse_ui_richtext_tests test_richtext.cpp)
    target_link_libraries(fuse_ui_richtext_tests PRIVATE fuse_ui_extras)
    target_include_directories(fuse_ui_richtext_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
    foreach(g model undo widget view)
        add_test(NAME fuse_ui_extras_richtext_${g} COMMAND fuse_ui_richtext_tests --gate=${g})
        set_tests_properties(fuse_ui_extras_richtext_${g} PROPERTIES LABELS "fuse_ui;gate")
    endforeach()
    if(NOT FUSE_UI_TEXT STREQUAL "FULL") # the golden is rendered with the MINIMAL text profile
        add_test(NAME fuse_ui_extras_richtext_golden COMMAND fuse_ui_richtext_tests --gate=golden)
        set_tests_properties(fuse_ui_extras_richtext_golden PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()

# ---- binary size (plan section 6: measure, then hold within +10 %) -----------------------------------------------
# fuse_app_binary_size: the stripped hello_app. The budget is per toolchain: baseline + 10 %. Linux x86-64, GCC 13.3,
# Release, MINIMAL text, standalone UI tree, measured 2026-11-13 on main: 1,936,432 bytes -> 2,130,000. Other
# toolchains / build types report the size and skip (77) until -DFUSE_APP_BINARY_BUDGET=<bytes> is set. Re-measure
# when the widget library grows on purpose (docs/ui/BASELINES.md).
if(TARGET hello_app AND NOT CMAKE_CROSSCOMPILING)
    set(_fuse_app_budget 0)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND FUSE_UI_TEXT STREQUAL "MINIMAL"
       AND CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       AND CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR) # the standalone UI tree
        set(_fuse_app_budget 2130000)
    endif()
    if(FUSE_APP_BINARY_BUDGET)
        set(_fuse_app_budget ${FUSE_APP_BINARY_BUDGET})
    endif()
    set(_fuse_app_bin $<TARGET_FILE:hello_app>)
    add_executable(fuse_app_binary_size test_app_binary_size.cpp)
    if(NOT MSVC AND CMAKE_STRIP)
        set(_fuse_app_bin ${CMAKE_CURRENT_BINARY_DIR}/hello_app.stripped)
        add_custom_target(fuse_app_hello_stripped ALL
            COMMAND ${CMAKE_STRIP} -o ${_fuse_app_bin} $<TARGET_FILE:hello_app> DEPENDS hello_app VERBATIM)
        add_dependencies(fuse_app_binary_size fuse_app_hello_stripped)
    endif()
    add_test(NAME fuse_app_binary_size COMMAND fuse_app_binary_size --file=${_fuse_app_bin} --budget=${_fuse_app_budget}
             "--profile=${CMAKE_CXX_COMPILER_ID} ${CMAKE_BUILD_TYPE}, text ${FUSE_UI_TEXT}")
    set_tests_properties(fuse_app_binary_size PROPERTIES SKIP_RETURN_CODE 77 LABELS "fuse_ui;gate;perf")
endif()

# ---- project.json schema 2 + AppPresent + the minimal init path (in tree: fuse_app_project needs fuse_project) ------
# fuse_project_schema2: schema 1 files unchanged, schema 2 round trip and validation, the Application template, an app
# project through ProjectHost (headless), ui.theme. With the engine game side (fuse_app_project_hybrid) also the
# deferred game side of a hybrid project; its GPU viewport and the --native run need a Vulkan device.
if(TARGET fuse_app_project)
    add_executable(fuse_project_schema2 test_project_schema2.cpp)
    target_compile_definitions(fuse_project_schema2 PRIVATE FUSE_REPO_ROOT="${FUSE_UI_REPO_ROOT}")
    if(TARGET fuse_app_project_hybrid)
        target_link_libraries(fuse_project_schema2 PRIVATE fuse_app_project_hybrid fuse_core)
        target_compile_definitions(fuse_project_schema2 PRIVATE FUSE_PROJECT_SCHEMA2_HYBRID=1)
    else()
        target_link_libraries(fuse_project_schema2 PRIVATE fuse_app_project fuse_core)
    endif()
    add_test(NAME fuse_project_schema2 COMMAND fuse_project_schema2)
    set_tests_properties(fuse_project_schema2 PROPERTIES LABELS "fuse_ui;gate;project" TIMEOUT 300 RUN_SERIAL TRUE)
    add_test(NAME fuse_project_schema2_native COMMAND fuse_project_schema2 --native)
    set_tests_properties(fuse_project_schema2_native PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300
        LABELS "fuse_ui;gate;project;vulkan")
endif()

# ---- Extras PDF: vector paths of a draw list as PDF path operators --------------------------------------------------
if(TARGET fuse_ui_extras)
    fuse_ui_test(fuse_ui_extras_pdf_paths test_pdf_paths.cpp)
    target_link_libraries(fuse_ui_extras_pdf_paths PRIVATE fuse_ui_extras)
    set_tests_properties(fuse_ui_extras_pdf_paths PROPERTIES LABELS "fuse_ui;gate")
endif()
