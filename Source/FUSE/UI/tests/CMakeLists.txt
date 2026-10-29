# FUSE UI2 gates (FUSE_UI_PLAN.md §9 UI2). One executable, one ctest per gate.
add_executable(fuse_ui_core_tests test_ui_core.cpp)
target_link_libraries(fuse_ui_core_tests PRIVATE fuse_ui_core)
foreach(gate layout_conformance style_cascade event_routing focus_order cpu_raster_golden zero_alloc_steady
             bench_10k a11y_tree_dump immediate timers_commands)
    add_test(NAME fuse_ui_${gate} COMMAND fuse_ui_core_tests ${gate})
    set_tests_properties(fuse_ui_${gate} PROPERTIES LABELS "ui;gate" TIMEOUT 60)
endforeach()
