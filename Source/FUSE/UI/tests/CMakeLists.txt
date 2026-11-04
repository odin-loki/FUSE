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
    elseif(FUSE_UI_TEXT STREQUAL "COOKED")
        target_compile_definitions(${name} PRIVATE FUSE_UI_TEST_TEXT_COOKED=1
                                   FUSE_UI_TEST_COOKED_FONTS="${CMAKE_CURRENT_BINARY_DIR}/fonts")
        add_dependencies(${name} fuse_ui_test_fonts)
    endif()
endfunction()

# COOKED: the test-data fonts cooked at the sizes their gates draw (colour images included)
if(FUSE_UI_TEXT STREQUAL "COOKED")
    set(_tf "${CMAKE_CURRENT_BINARY_DIR}/fonts")
    set(_tf_out "")
    foreach(spec "colrv1_test|--sizes=30,32,45,48,56|--msdf"
                 "bitmap_cbdt_test|--sizes=20|--color-sizes=8,12,16,20,32,40,64,100"
                 "bitmap_sbix_test|--sizes=20|--color-sizes=8,12,16,20,32,40,64,100")
        string(REPLACE "|" ";" _spec "${spec}")
        list(POP_FRONT _spec _name)
        add_custom_command(OUTPUT "${_tf}/${_name}.fusefont"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_tf}"
            COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:fuse_text_cook> "--out=${_tf}/${_name}.fusefont" ${_spec}
                    "${CMAKE_CURRENT_SOURCE_DIR}/data/${_name}.ttf"
            DEPENDS fuse_text_cook "${CMAKE_CURRENT_SOURCE_DIR}/data/${_name}.ttf" COMMENT "fuse_text_cook ${_name}" VERBATIM)
        list(APPEND _tf_out "${_tf}/${_name}.fusefont")
    endforeach()
    add_custom_target(fuse_ui_test_fonts DEPENDS ${_tf_out})
endif()

fuse_ui_test(fuse_ui_layout_conformance test_layout_conformance.cpp)
fuse_ui_test(fuse_ui_style_cascade test_style_cascade.cpp)
fuse_ui_test(fuse_ui_event_routing test_event_routing.cpp)
fuse_ui_test(fuse_ui_focus_order test_focus_order.cpp)
fuse_ui_test(fuse_ui_cpu_raster_golden test_cpu_raster_golden.cpp)
set_tests_properties(fuse_ui_cpu_raster_golden PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
fuse_ui_test(fuse_ui_zero_alloc_steady test_zero_alloc_steady.cpp)
fuse_ui_test(fuse_ui_bench_10k test_bench_10k.cpp)
set_tests_properties(fuse_ui_bench_10k PROPERTIES RUN_SERIAL TRUE) # timing budget: not beside other tests
fuse_ui_test(fuse_ui_single_thread_parity test_single_thread_parity.cpp)
fuse_ui_test(fuse_ui_a11y_tree_dump test_a11y_tree_dump.cpp)
fuse_ui_test(fuse_ui_text_basics test_text_basics.cpp)
fuse_ui_test(fuse_text_edit_ops test_text_edit_ops.cpp)
fuse_ui_test(fuse_text_fallback test_text_fallback.cpp)
fuse_ui_test(fuse_ui_widget_interaction test_widget_interaction.cpp)

# UI3 text gates: Unicode conformance files (tests/data/unicode), per-script goldens, system fonts, zero-alloc, DPI.
foreach(g grapheme linebreak bidi)
    fuse_ui_test(fuse_text_${g}_conformance test_text_conformance.cpp)
    target_compile_definitions(fuse_text_${g}_conformance PRIVATE FUSE_TEXT_CONFORMANCE_GATE="${g}"
                               FUSE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    set_tests_properties(fuse_text_${g}_conformance PROPERTIES LABELS "fuse_ui;gate;text" SKIP_RETURN_CODE 77)
endforeach()
foreach(g shaping_scripts system_fonts cache_zero_alloc colrv1 dpi_1 dpi_1_25 dpi_1_5 dpi_2)
    fuse_ui_test(fuse_text_${g} test_text_ui3.cpp)
    target_compile_definitions(fuse_text_${g} PRIVATE FUSE_TEXT_GATE="${g}")
    set_tests_properties(fuse_text_${g} PROPERTIES LABELS "fuse_ui;gate;text" SKIP_RETURN_CODE 77
                         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# COOKED profile: cooker determinism (every profile), cooked layout / renders against the MINIMAL reference (COOKED)
fuse_ui_test(fuse_text_cooked test_text_cooked.cpp)
string(REPLACE ";" " " _cook_args "${FUSE_TEXT_UI_COOK_ARGS}")
target_include_directories(fuse_text_cooked PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Text/tools)
target_compile_definitions(fuse_text_cooked PRIVATE FUSE_TEXT_COOK_EXE="$<TARGET_FILE:fuse_text_cook>"
    FUSE_TEXT_TEST_FONT="${FUSE_UI_REPO_ROOT}/vendor/fonts/opensans/OpenSans.ttf" FUSE_TEXT_TEST_COOK_ARGS="${_cook_args}"
    FUSE_TEXT_TEST_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(fuse_text_cooked fuse_text_cook)
if(FUSE_UI_TEXT STREQUAL "COOKED")
    target_compile_definitions(fuse_text_cooked PRIVATE
        FUSE_TEXT_COOKED_PACK="${FUSE_TEXT_COOKED_FONT_DIR}/opensans/OpenSans.fusefont"
        FUSE_TEXT_COOKED_REFERENCE="${FUSE_TEXT_COOKED_FONT_DIR}/opensans/OpenSans.reference")
endif()
set_tests_properties(fuse_text_cooked PROPERTIES LABELS "fuse_ui;gate;text")

# Colour glyphs (CBDT / sbix strikes) and MSDF glyphs (quality, UI threshold, GPU parity when fuse_ui_vk is built).
foreach(g bitmap_emoji msdf)
    fuse_ui_test(fuse_text_${g} test_text_color.cpp)
    target_compile_definitions(fuse_text_${g} PRIVATE FUSE_TEXT_GATE="${g}")
    set_tests_properties(fuse_text_${g} PROPERTIES LABELS "fuse_ui;gate;text" SKIP_RETURN_CODE 77
                         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ENVIRONMENT "${FUSE_UI_LVP_ENV}")
    if(TARGET fuse_ui_vk)
        target_link_libraries(fuse_text_${g} PRIVATE fuse_ui_vk ${FUSE_UI_VK_LINK})
        target_compile_definitions(fuse_text_${g} PRIVATE FUSE_TEXT_COLOR_HAVE_VK=1)
    endif()
endforeach()

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
# UI1 platform services, UI side: DPI scale, cursor style, reduced motion, touch / pen, theme slot.
fuse_ui_test(fuse_ui_platform_services test_platform_services.cpp ${FUSE_CORE_DIR}/src/platform/input.cpp
             ${FUSE_CORE_DIR}/src/platform/text_services.cpp)
target_include_directories(fuse_ui_platform_services PRIVATE ${FUSE_CORE_DIR}/include)
set_tests_properties(fuse_ui_platform_services PROPERTIES LABELS "fuse_ui;gate")
if(WIN32) # text_services.cpp uses IMM32 (ImmAssociateContextEx etc.)
    target_link_libraries(fuse_ui_platform_keys_v2 PRIVATE imm32)
    target_link_libraries(fuse_ui_text_input PRIVATE imm32)
    target_link_libraries(fuse_ui_platform_services PRIVATE imm32)
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
endif()

add_executable(fuse_ui_sample sample_main.cpp)
target_link_libraries(fuse_ui_sample PRIVATE fuse_ui_widgets)
target_include_directories(fuse_ui_sample PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})

# multi-colour icon test pack (tests/data/icons_color), cooked by the tool like the starter pack
if(TARGET fuse_ui_icons)
    set(FUSE_UI_ICON_COLOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/icons_color")
    set(FUSE_UI_ICON_COLOR_PACK "${CMAKE_CURRENT_BINARY_DIR}/icons_color.fuseicons")
    file(GLOB _fuse_ui_color_svgs CONFIGURE_DEPENDS "${FUSE_UI_ICON_COLOR_DIR}/*.svg")
    add_custom_command(OUTPUT "${FUSE_UI_ICON_COLOR_PACK}"
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:fuse_ui_icon_cook> "--out=${FUSE_UI_ICON_COLOR_PACK}" "${FUSE_UI_ICON_COLOR_DIR}"
        DEPENDS fuse_ui_icon_cook ${_fuse_ui_color_svgs}
        COMMENT "fuse_ui_icon_cook: multi-colour test icons" VERBATIM)
    add_custom_target(fuse_ui_icons_color DEPENDS "${FUSE_UI_ICON_COLOR_PACK}")
endif()

# UI4 gate: GPU draw-list backend vs the CPU reference (lavapipe / any Vulkan device; 77 = skipped, no device).
if(TARGET fuse_ui_vk)
    fuse_ui_test(fuse_ui_vk_parity test_vk_parity.cpp)
    target_link_libraries(fuse_ui_vk_parity PRIVATE fuse_ui_vk ${FUSE_UI_VK_LINK})
    set_tests_properties(fuse_ui_vk_parity PROPERTIES SKIP_RETURN_CODE 77 LABELS "ui;gpu")
    foreach(g damage validation_clean budget)
        fuse_ui_test(fuse_ui_vk_${g} test_vk_${g}.cpp)
        target_link_libraries(fuse_ui_vk_${g} PRIVATE fuse_ui_vk ${FUSE_UI_VK_LINK})
        set_tests_properties(fuse_ui_vk_${g} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${FUSE_UI_LVP_ENV}"
                             LABELS "fuse_ui;gate;vulkan")
    endforeach()
    # custom shader effects (built-ins + a user effect through fuse_ui_add_effect) and the SDF-quad backend
    foreach(g custom_effect sdf_quads)
        fuse_ui_test(fuse_ui_vk_${g} test_vk_${g}.cpp)
        target_link_libraries(fuse_ui_vk_${g} PRIVATE fuse_ui_vk ${FUSE_UI_VK_LINK})
        set_tests_properties(fuse_ui_vk_${g} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${FUSE_UI_LVP_ENV}"
                             LABELS "fuse_ui;gate;vulkan")
    endforeach()
    if(COMMAND fuse_ui_add_effect)
        fuse_ui_add_effect(fuse_ui_vk_custom_effect NAME kTestFxVignette SOURCE data/fx_vignette_image.glsl)
    endif()
    if(TARGET fuse_ui_icons) # icons cases: the build-time starter pack and the multi-colour test pack
        add_dependencies(fuse_ui_vk_parity fuse_ui_icons fuse_ui_icons_color)
        target_compile_definitions(fuse_ui_vk_parity PRIVATE FUSE_UI_ICON_PACK="${FUSE_UI_ICON_PACK}"
                                   FUSE_UI_ICON_COLOR_PACK="${FUSE_UI_ICON_COLOR_PACK}")
    endif()
endif()

fuse_ui_test(fuse_ui_text_full test_text_full.cpp)
fuse_ui_test(fuse_ui_tier_b test_tier_b.cpp)
fuse_ui_test(fuse_ui_tier_b2 test_tier_b2.cpp)
fuse_ui_test(fuse_ui_backdrop_blur test_backdrop_blur.cpp)
# UI4 icons: SVG cooker determinism / index round trip, CPU raster golden of icons (tests/golden/icons.png)
if(TARGET fuse_ui_icons)
    # test name = gate name; the cook gate's executable is fuse_ui_icon_cook_gate (fuse_ui_icon_cook is the tool)
    foreach(pair "cook=fuse_ui_icon_cook=fuse_ui_icon_cook_gate" "render=fuse_ui_icon_render=fuse_ui_icon_render")
        string(REPLACE "=" ";" kv ${pair})
        list(GET kv 0 g)
        list(GET kv 1 name)
        list(GET kv 2 t)
        add_executable(${t} test_icons.cpp)
        target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
        add_test(NAME ${name} COMMAND ${t})
        target_link_libraries(${t} PRIVATE fuse_ui_widgets fuse_ui_icon_cook_lib)
        add_dependencies(${t} fuse_ui_icons fuse_ui_icons_color)
        target_compile_definitions(${t} PRIVATE FUSE_UI_ICON_GATE="${g}" FUSE_UI_ICON_PACK="${FUSE_UI_ICON_PACK}"
                                   FUSE_UI_ICON_SVG_DIR="${FUSE_UI_ICON_SVG_DIR}"
                                   FUSE_UI_ICON_COLOR_PACK="${FUSE_UI_ICON_COLOR_PACK}"
                                   FUSE_UI_ICON_COLOR_DIR="${FUSE_UI_ICON_COLOR_DIR}")
        if(TARGET fuse_ui_doc)
            target_link_libraries(${t} PRIVATE fuse_ui_doc)
            target_compile_definitions(${t} PRIVATE FUSE_UI_ICON_HAVE_DOC=1)
        endif()
        set_tests_properties(${name} PROPERTIES LABELS "fuse_ui;gate" WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
endif()
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
    set_tests_properties(fuse_app_hello_startup PROPERTIES RUN_SERIAL TRUE) # timing budget
    if(TARGET fuse_app_native)
        add_executable(fuse_app_native_window test_app_native.cpp)
        target_link_libraries(fuse_app_native_window PRIVATE fuse_app_native)
        find_program(FUSE_UI_XVFB_RUN xvfb-run)
        # +bs: backing store, so XGetImage reads windows that overlap (no window manager places them under Xvfb)
        set(FUSE_UI_XVFB_ARGS "-screen 0 1280x800x24 +bs")
        if(FUSE_UI_XVFB_RUN AND NOT WIN32)
            add_test(NAME fuse_app_native_window COMMAND ${FUSE_UI_XVFB_RUN} -a -s "${FUSE_UI_XVFB_ARGS}"
                     $<TARGET_FILE:fuse_app_native_window> --out=${CMAKE_CURRENT_BINARY_DIR})
        else()
            add_test(NAME fuse_app_native_window COMMAND fuse_app_native_window --out=${CMAKE_CURRENT_BINARY_DIR})
        endif()
        set_tests_properties(fuse_app_native_window PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300
            ENVIRONMENT "${FUSE_UI_LVP_ENV}" LABELS "fuse_ui;gate;vulkan")
        add_executable(fuse_app_os_popups test_app_os_popups.cpp)
        target_link_libraries(fuse_app_os_popups PRIVATE fuse_app_native)
        if(FUSE_UI_XVFB_RUN AND NOT WIN32)
            add_test(NAME fuse_app_os_popups COMMAND ${FUSE_UI_XVFB_RUN} -a -s "${FUSE_UI_XVFB_ARGS}"
                     $<TARGET_FILE:fuse_app_os_popups> --out=${CMAKE_CURRENT_BINARY_DIR})
        else()
            add_test(NAME fuse_app_os_popups COMMAND fuse_app_os_popups --out=${CMAKE_CURRENT_BINARY_DIR})
        endif()
        set_tests_properties(fuse_app_os_popups PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300
            ENVIRONMENT "${FUSE_UI_LVP_ENV}" LABELS "fuse_ui;gate;vulkan")
        # §6 idle budget on a real window: zero presents, ~0 % CPU, a handful of loop wakeups over 3 s
        add_executable(fuse_app_idle_native test_app_idle_native.cpp)
        target_link_libraries(fuse_app_idle_native PRIVATE fuse_app_native)
        if(FUSE_UI_XVFB_RUN AND NOT WIN32)
            add_test(NAME fuse_app_idle_native COMMAND ${FUSE_UI_XVFB_RUN} -a -s "${FUSE_UI_XVFB_ARGS}"
                     $<TARGET_FILE:fuse_app_idle_native>)
        else()
            add_test(NAME fuse_app_idle_native COMMAND fuse_app_idle_native)
        endif()
        set_tests_properties(fuse_app_idle_native PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 120
            ENVIRONMENT "${FUSE_UI_LVP_ENV}" LABELS "fuse_ui;gate;vulkan")
        # custom window chrome (WM_NCHITTEST / _NET_WM_MOVERESIZE on a real window) and tray icon + notifications (UI10).
        # Unix tray: StatusNotifierItem on a private session bus (dbus-run-session; the gate plays the desktop's side).
        find_program(FUSE_UI_DBUS_RUN_SESSION dbus-run-session)
        foreach(g window_chrome tray)
            set(_src ${g})
            if(g STREQUAL "window_chrome")
                set(_src chrome)
            endif()
            add_executable(fuse_app_${g} test_app_${_src}.cpp)
            target_link_libraries(fuse_app_${g} PRIVATE fuse_app_native)
            target_include_directories(fuse_app_${g} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../App/src) # X11 / D-Bus loaders
            set(_bus "")
            if(g STREQUAL "tray" AND FUSE_UI_DBUS_RUN_SESSION AND NOT WIN32)
                set(_bus ${FUSE_UI_DBUS_RUN_SESSION} --)
            endif()
            if(FUSE_UI_XVFB_RUN AND NOT WIN32)
                add_test(NAME fuse_app_${g} COMMAND ${_bus} ${FUSE_UI_XVFB_RUN} -a -s "${FUSE_UI_XVFB_ARGS}"
                         $<TARGET_FILE:fuse_app_${g}> --out=${CMAKE_CURRENT_BINARY_DIR})
            else()
                add_test(NAME fuse_app_${g} COMMAND fuse_app_${g} --out=${CMAKE_CURRENT_BINARY_DIR})
            endif()
            set_tests_properties(fuse_app_${g} PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 120 LABELS "fuse_ui;gate")
            if(_bus)
                set_tests_properties(fuse_app_${g} PROPERTIES ENVIRONMENT "FUSE_TEST_PRIVATE_BUS=1")
            endif()
        endforeach()
    endif()
    # project.json schema 2 + AppPresent + minimal init path (in tree: needs fuse_project / fuse_core)
    if(TARGET fuse_app_project)
        add_executable(fuse_project_schema2 test_project_schema2.cpp)
        target_link_libraries(fuse_project_schema2 PRIVATE fuse_app_project_hybrid fuse_core)
        target_compile_definitions(fuse_project_schema2 PRIVATE FUSE_REPO_ROOT="${FUSE_UI_REPO_ROOT}")
        add_test(NAME fuse_project_schema2 COMMAND fuse_project_schema2)
        # the hybrid viewport renders its GPU scene (Lavapipe on Linux CI)
        set_tests_properties(fuse_project_schema2 PROPERTIES LABELS "fuse_ui;gate;project;vulkan" TIMEOUT 300
            RUN_SERIAL TRUE ENVIRONMENT "${FUSE_UI_LVP_ENV}")
        if(FUSE_UI_XVFB_RUN AND NOT WIN32)
            add_test(NAME fuse_project_schema2_native COMMAND ${FUSE_UI_XVFB_RUN} -a -s "${FUSE_UI_XVFB_ARGS}"
                     $<TARGET_FILE:fuse_project_schema2> --native)
        else()
            add_test(NAME fuse_project_schema2_native COMMAND fuse_project_schema2 --native)
        endif()
        set_tests_properties(fuse_project_schema2_native PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300
            ENVIRONMENT "${FUSE_UI_LVP_ENV}" LABELS "fuse_ui;gate;project;vulkan")
    endif()
    # Windows UI Automation through AccessKit on a real window (skips with 77 when AccessKit is not linked)
    if(TARGET fuse_app_a11y AND WIN32)
        add_executable(fuse_ui_a11y_windows test_a11y_windows.cpp)
        target_link_libraries(fuse_ui_a11y_windows PRIVATE fuse_app_a11y ole32 oleaut32)
        add_test(NAME fuse_ui_a11y_windows COMMAND fuse_ui_a11y_windows)
        set_tests_properties(fuse_ui_a11y_windows PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 120
            LABELS "fuse_ui;gate;a11y")
    endif()
    # §6 budgets on the hello_app sample: launch -> first presented frame on a real OS window, and binary size
    if(TARGET hello_app)
        add_executable(fuse_app_native_startup test_app_native_startup.cpp)
        add_test(NAME fuse_app_native_startup COMMAND fuse_app_native_startup $<TARGET_FILE:hello_app> --runs=5 --budget-ms=150
                 --vulkan-budget-ms=2000)
        set_tests_properties(fuse_app_native_startup PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 120
            LABELS "fuse_ui;gate")
        # Baseline per toolchain + text profile + build kind, +10 % (plan §6). MSVC 19.5x x64 Release, MINIMAL text,
        # measured 2026-11-05 (MSVC Build Tools 2026) after the colour / MSDF glyph modes and the pipeline cache:
        # standalone UI tree 870,912 bytes -> budget 958,000; in the engine tree hello_app also links fuse_rhi's
        # volk loader: 904,192 bytes -> budget 994,600. Other toolchains: set FUSE_APP_BINARY_BUDGET after measuring
        # (the gate then checks a stripped copy); until then it reports and skips.
        set(_budget 0)
        if(MSVC AND FUSE_UI_TEXT STREQUAL "MINIMAL" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
            if(TARGET fuse_rhi)
                set(_budget 994600)
            else()
                set(_budget 958000)
            endif()
        endif()
        if(FUSE_APP_BINARY_BUDGET) # -DFUSE_APP_BINARY_BUDGET=<bytes> overrides
            set(_budget ${FUSE_APP_BINARY_BUDGET})
        endif()
        set(_bin $<TARGET_FILE:hello_app>)
        add_executable(fuse_app_binary_size test_app_binary_size.cpp)
        if(NOT MSVC AND CMAKE_STRIP)
            # a custom target here: add_custom_command(TARGET hello_app) from another directory needs CMake 3.31
            set(_bin ${CMAKE_CURRENT_BINARY_DIR}/hello_app.stripped)
            add_custom_target(fuse_app_hello_stripped ALL
                COMMAND ${CMAKE_STRIP} -o ${_bin} $<TARGET_FILE:hello_app> DEPENDS hello_app VERBATIM)
            add_dependencies(fuse_app_binary_size fuse_app_hello_stripped)
        endif()
        add_test(NAME fuse_app_binary_size COMMAND fuse_app_binary_size --file=${_bin} --budget=${_budget}
                 "--profile=${CMAKE_CXX_COMPILER_ID} ${CMAKE_BUILD_TYPE}, text ${FUSE_UI_TEXT}")
        set_tests_properties(fuse_app_binary_size PROPERTIES SKIP_RETURN_CODE 77 LABELS "fuse_ui;gate")
    endif()
    if(TARGET game_menu)
        add_test(NAME fuse_app_game_menu_sample COMMAND game_menu --script --screenshot=${CMAKE_CURRENT_BINARY_DIR}/game_menu.png)
        set_tests_properties(fuse_app_game_menu_sample PROPERTIES LABELS fuse_ui)
    endif()
    if(TARGET fuse_app_native)
        # UI1 desktop platform services on real windows: DPI change, displays, cursors, system theme, native dialogs,
        # touch injection, multiple windows (one executable per gate; see test_platform_desktop.cpp).
        foreach(g dpi_change displays cursors system_theme dialogs touch_synthetic multiwindow)
            add_executable(fuse_platform_${g} test_platform_desktop.cpp)
            target_link_libraries(fuse_platform_${g} PRIVATE fuse_app_native)
            target_compile_definitions(fuse_platform_${g} PRIVATE FUSE_PLATFORM_GATE="${g}")
            if(FUSE_UI_XVFB_RUN AND NOT WIN32)
                add_test(NAME fuse_platform_${g} COMMAND ${FUSE_UI_XVFB_RUN} -a -s "${FUSE_UI_XVFB_ARGS}"
                         $<TARGET_FILE:fuse_platform_${g}>)
            else()
                add_test(NAME fuse_platform_${g} COMMAND fuse_platform_${g})
            endif()
            set_tests_properties(fuse_platform_${g} PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180
                ENVIRONMENT "${FUSE_UI_LVP_ENV}" LABELS "fuse_ui;gate;vulkan")
        endforeach()
        # X11 parity (RandR monitors, XSETTINGS DPI / theme, pump wake) against the gate's own X connection
        if(FUSE_UI_XVFB_RUN AND NOT WIN32)
            foreach(g randr xsettings wake)
                add_executable(fuse_x11_${g} test_platform_x11.cpp)
                target_link_libraries(fuse_x11_${g} PRIVATE fuse_app_native)
                target_include_directories(fuse_x11_${g} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../App/src)
                target_compile_definitions(fuse_x11_${g} PRIVATE FUSE_X11_GATE="${g}")
                if(g STREQUAL "wake" AND TARGET fuse_app_a11y)
                    target_link_libraries(fuse_x11_${g} PRIVATE fuse_app_a11y)
                    target_compile_definitions(fuse_x11_${g} PRIVATE FUSE_X11_GATE_A11Y=1)
                endif()
                add_test(NAME fuse_x11_${g} COMMAND ${FUSE_UI_XVFB_RUN} -a -s "${FUSE_UI_XVFB_ARGS}" $<TARGET_FILE:fuse_x11_${g}>)
                set_tests_properties(fuse_x11_${g} PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 120
                    ENVIRONMENT "${FUSE_UI_LVP_ENV}" LABELS "fuse_ui;gate")
            endforeach()
        endif()
    endif()
    if(TARGET hello_app)
        add_test(NAME fuse_app_hello_sample COMMAND hello_app --screenshot=${CMAKE_CURRENT_BINARY_DIR}/hello_app.png)
        add_test(NAME fuse_app_notes_sample COMMAND notes_app --settings=${CMAKE_CURRENT_BINARY_DIR}/notes_settings.ini
                 --screenshot=${CMAKE_CURRENT_BINARY_DIR}/notes_app.png)
        set_tests_properties(fuse_app_hello_sample fuse_app_notes_sample PROPERTIES LABELS fuse_ui)
    endif()
endif()

# §6 / UI0 baselines (report only): CPU raster + GPU time of typical frames, atlas and GPU memory
if(TARGET fuse_ui_game AND TARGET fuse_ui_vk)
    fuse_ui_test(fuse_ui_baselines test_ui_baselines.cpp)
    target_link_libraries(fuse_ui_baselines PRIVATE fuse_ui_game fuse_ui_vk ${FUSE_UI_VK_LINK})
    if(TARGET fuse_ui_icons)
        add_dependencies(fuse_ui_baselines fuse_ui_icons)
        target_compile_definitions(fuse_ui_baselines PRIVATE FUSE_UI_ICON_PACK="${FUSE_UI_ICON_PACK}")
    endif()
    set_tests_properties(fuse_ui_baselines PROPERTIES ENVIRONMENT "${FUSE_UI_LVP_ENV}" LABELS "fuse_ui;report")
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
