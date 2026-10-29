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
fuse_ui_test(fuse_ui_widget_interaction test_widget_interaction.cpp)

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

# UI4 gate: GPU draw-list backend vs the CPU reference (lavapipe / any Vulkan device; 77 = skipped, no device).
if(TARGET fuse_ui_vk)
    fuse_ui_test(fuse_ui_vk_parity test_vk_parity.cpp)
    target_link_libraries(fuse_ui_vk_parity PRIVATE fuse_ui_vk ${FUSE_UI_VK_LINK})
    set_tests_properties(fuse_ui_vk_parity PROPERTIES SKIP_RETURN_CODE 77 LABELS "ui;gpu")
endif()

fuse_ui_test(fuse_ui_text_full test_text_full.cpp)
fuse_ui_test(fuse_ui_tier_b test_tier_b.cpp)
fuse_ui_test(fuse_ui_tier_b2 test_tier_b2.cpp)

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
    endif()
    if(TARGET hello_app)
        add_test(NAME fuse_app_hello_sample COMMAND hello_app --screenshot=${CMAKE_CURRENT_BINARY_DIR}/hello_app.png)
        add_test(NAME fuse_app_notes_sample COMMAND notes_app --settings=${CMAKE_CURRENT_BINARY_DIR}/notes_settings.ini
                 --screenshot=${CMAKE_CURRENT_BINARY_DIR}/notes_app.png)
        set_tests_properties(fuse_app_hello_sample fuse_app_notes_sample PROPERTIES LABELS fuse_ui)
    endif()
endif()
