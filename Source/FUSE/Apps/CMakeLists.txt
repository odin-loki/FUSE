if(FUSE_BUILD_PARITY_DEMOS AND FUSE_BUILD_PROJECT)
    add_subdirectory(Common)
endif()

if(FUSE_BUILD_SMOKE)
    add_subdirectory(RuntimeSmoke)
endif()

if(FUSE_BUILD_HYBRID_DEMO)
    add_subdirectory(HybridHud)
endif()

if(FUSE_BUILD_PARITY_DEMOS AND FUSE_BUILD_PROJECT)
    add_subdirectory(Demo3DEmpty)
    add_subdirectory(Demo2DSprites)
    add_subdirectory(DemoAiBt)
    add_subdirectory(DemoTimeline)
    add_subdirectory(DemoFx)
    add_subdirectory(DemoAdventureStub)

    if(FUSE_BUILD_CORE_TESTS)
        set(_fuse_parity_demo_root "${CMAKE_SOURCE_DIR}/Samples/unification")
        set(_fuse_parity_demo_tests
            demo_3d_empty
            demo_2d_sprites
            demo_ai_bt
            demo_timeline
            demo_fx
            demo_adventure_stub
        )
        # E03: demo_3d_empty / demo_2d_sprites render through the hybrid GPU scene path when Vulkan is present
        # (checked by GPU readback): they run on Lavapipe under the ICD lock like the other Vulkan gates.
        set(_fuse_demo_icd_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
        foreach(_demo ${_fuse_parity_demo_tests})
            if(_demo MATCHES "^demo_(3d_empty|2d_sprites)$" AND FUSE_VULKAN_BACKEND AND NOT CMAKE_CROSSCOMPILING
               AND EXISTS "${_fuse_demo_icd_lock}")
                add_test(NAME fuse_u8_${_demo}
                    COMMAND "${_fuse_demo_icd_lock}" "$<TARGET_FILE:${_demo}>" "${_fuse_parity_demo_root}/${_demo}")
                set_tests_properties(fuse_u8_${_demo} PROPERTIES
                    ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" RUN_SERIAL TRUE)
            else()
                # Target-name COMMAND so a cross build runs it through CMAKE_CROSSCOMPILING_EMULATOR.
                add_test(NAME fuse_u8_${_demo}
                    COMMAND ${_demo} "${_fuse_parity_demo_root}/${_demo}")
            endif()
            set_tests_properties(fuse_u8_${_demo} PROPERTIES
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            )
        endforeach()
        if(COMMAND fuse_apply_asan)
            foreach(_demo ${_fuse_parity_demo_tests})
                fuse_apply_asan(${_demo})
            endforeach()
            if(TARGET demo_hybrid_hud)
                fuse_apply_asan(demo_hybrid_hud)
            endif()
        endif()
    endif()
endif()

# B1.8 valgrind leak gate: memcheck twins (label "valgrind") of representative core/ECS/scene tests
# and fuse_runtime_smoke. Registered here because Apps is configured after every engine library.
include(${CMAKE_SOURCE_DIR}/cmake/FuseValgrind.cmake)
fuse_add_valgrind_tests()
