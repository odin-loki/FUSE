add_executable(demo_hybrid_hud
    main.cpp
    hybrid_module_gates.cpp
)

target_link_libraries(demo_hybrid_hud PRIVATE
    fuse_hybrid
    fuse_ai
    fuse_cinematics
    fuse_fx
    fuse_mechanics
    fuse_adventure
    fuse_core
)
if(FUSE_BUILD_SCRIPT)
    target_link_libraries(demo_hybrid_hud PRIVATE fuse_script)
endif()
target_compile_features(demo_hybrid_hud PRIVATE cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_hybrid_module_gates_tests
        test_hybrid_module_gates.cpp
        hybrid_module_gates.cpp
    )
    target_link_libraries(fuse_hybrid_module_gates_tests PRIVATE
        fuse_hybrid
        fuse_ai
        fuse_cinematics
        fuse_fx
        fuse_mechanics
        fuse_adventure
        fuse_core
    )
    if(FUSE_BUILD_SCRIPT)
        target_link_libraries(fuse_hybrid_module_gates_tests PRIVATE fuse_script)
    endif()
    target_compile_features(fuse_hybrid_module_gates_tests PRIVATE cxx_std_17)
    target_compile_definitions(fuse_hybrid_module_gates_tests PRIVATE FUSE_HYBRID_GATES_WAVE16=1)
    add_test(NAME fuse_hybrid_module_gates_tests COMMAND fuse_hybrid_module_gates_tests)
endif()

if(FUSE_SMOKE_ENABLE_ASAN)
    target_compile_options(demo_hybrid_hud PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(demo_hybrid_hud PRIVATE -fsanitize=address)
endif()
