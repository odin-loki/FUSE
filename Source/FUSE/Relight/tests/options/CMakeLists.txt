# FUSE Relight RL-0.6: options system tests (ctest rl_options).
# Semantics ported from dxvk-remix tests/rtx/unit/test_rtx_option.cpp and
# test_option_layer_export.cpp, plus .conf parsing, hash-set syntax and byte-stable save checks.

add_executable(fuse_relight_options_tests
    test_options_main.cpp
    test_options_config.cpp
    test_options_semantics.cpp
    test_options_export.cpp
    test_options_system.cpp
)
target_link_libraries(fuse_relight_options_tests PRIVATE fuse_relight_options)

add_test(NAME rl_options COMMAND fuse_relight_options_tests)
set_tests_properties(rl_options PROPERTIES LABELS "gate;relight" TIMEOUT 120)

# Process exit with dirty options and a static layer handle (static-destruction order regression).
add_executable(fuse_relight_options_exit_test test_options_exit.cpp)
target_link_libraries(fuse_relight_options_exit_test PRIVATE fuse_relight_options)
add_test(NAME rl_options_exit COMMAND fuse_relight_options_exit_test)
set_tests_properties(rl_options_exit PROPERTIES LABELS "gate;relight" TIMEOUT 60)
