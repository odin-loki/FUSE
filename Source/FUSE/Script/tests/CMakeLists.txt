add_executable(fuse_script_tests
    test_script_console.cpp
    test_script_host.cpp
)

target_link_libraries(fuse_script_tests PRIVATE fuse_script fuse_core)
add_test(NAME fuse_script_b73 COMMAND fuse_script_tests)

add_executable(fuse_script_compat_route_tests
    test_script_compat_route.cpp
)

target_link_libraries(fuse_script_compat_route_tests PRIVATE fuse_script fuse_core)
add_test(NAME fuse_script_compat_route COMMAND fuse_script_compat_route_tests)

if(TARGET fuse_script_physics)
    add_executable(fuse_script_b7_gate_tests
        test_b7_script_gates.cpp
    )
    target_link_libraries(fuse_script_b7_gate_tests PRIVATE fuse_script_physics fuse_script fuse_core)
    add_test(NAME fuse_script_b7_gates COMMAND fuse_script_b7_gate_tests)
    set_tests_properties(fuse_script_b7_gates PROPERTIES LABELS "gate")
    if(COMMAND fuse_apply_asan)
        fuse_apply_asan(fuse_script_b7_gate_tests)
    endif()
endif()

add_executable(fuse_script_b7_perf_tests
    test_b7_script_perf.cpp
)
target_link_libraries(fuse_script_b7_perf_tests PRIVATE fuse_script fuse_core)
add_test(NAME fuse_script_b7_perf COMMAND fuse_script_b7_perf_tests)
set_tests_properties(fuse_script_b7_perf PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")

if(MINGW)
    target_link_options(fuse_script_tests PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(fuse_script_compat_route_tests PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(fuse_script_b7_perf_tests PRIVATE -Wl,--allow-multiple-definition)
    if(TARGET fuse_script_b7_gate_tests)
        target_link_options(fuse_script_b7_gate_tests PRIVATE -Wl,--allow-multiple-definition)
    endif()
endif()
