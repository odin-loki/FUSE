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
    target_compile_definitions(fuse_hybrid_module_gates_tests PRIVATE
        FUSE_HYBRID_GATES_WAVE16=1
        FUSE_HYBRID_GATES_WAVE17=1
        FUSE_HYBRID_GATES_WAVE18=1
        FUSE_HYBRID_GATES_WAVE19=1
    )
    if(FUSE_BUILD_VULKAN)
        set(FUSE_HYBRID_GATES_VULKAN_ICD_LOCK_SCRIPT
            "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
        set(FUSE_HYBRID_GATES_VULKAN_TEST_ENV "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
        add_test(NAME fuse_hybrid_module_gates_tests
            COMMAND "${FUSE_HYBRID_GATES_VULKAN_ICD_LOCK_SCRIPT}"
                    "$<TARGET_FILE:fuse_hybrid_module_gates_tests>")
        set_tests_properties(fuse_hybrid_module_gates_tests PROPERTIES
            ENVIRONMENT "${FUSE_HYBRID_GATES_VULKAN_TEST_ENV}"
            RUN_SERIAL TRUE
        )
    else()
        add_test(NAME fuse_hybrid_module_gates_tests COMMAND fuse_hybrid_module_gates_tests)
    endif()
endif()

if(FUSE_SMOKE_ENABLE_ASAN)
    target_compile_options(demo_hybrid_hud PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(demo_hybrid_hud PRIVATE -fsanitize=address)
endif()
