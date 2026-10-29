# FUSE UI gates (plan §8) and extra tests. Every gate is a plain executable registered with ctest.
set(FUSE_UI_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(FUSE_UI_STB ${FUSE_UI_REPO_ROOT}/vendor/stb)

function(fuse_ui_test name src)
    add_executable(${name} ${src} ${ARGN})
    target_link_libraries(${name} PRIVATE fuse_ui_widgets)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FUSE_UI_STB})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS fuse_ui)
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
fuse_ui_test(fuse_ui_platform_keys_v2 test_platform_keys_v2.cpp ${FUSE_CORE_DIR}/src/platform/input.cpp)
target_include_directories(fuse_ui_platform_keys_v2 PRIVATE ${FUSE_CORE_DIR}/include)

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
