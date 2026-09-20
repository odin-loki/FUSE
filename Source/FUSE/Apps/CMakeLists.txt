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
        foreach(_demo ${_fuse_parity_demo_tests})
            add_test(NAME fuse_u8_${_demo}
                COMMAND $<TARGET_FILE:${_demo}> "${_fuse_parity_demo_root}/${_demo}")
            set_tests_properties(fuse_u8_${_demo} PROPERTIES
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            )
        endforeach()
        if(FUSE_SMOKE_ENABLE_ASAN)
            foreach(_demo ${_fuse_parity_demo_tests})
                target_compile_options(${_demo} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
                target_link_options(${_demo} PRIVATE -fsanitize=address)
            endforeach()
            target_compile_options(demo_hybrid_hud PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(demo_hybrid_hud PRIVATE -fsanitize=address)
        endif()
    endif()
endif()
