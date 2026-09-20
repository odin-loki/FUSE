add_executable(fuse_compat_ts_tests
    test_compat_ts.cpp
)

target_link_libraries(fuse_compat_ts_tests PRIVATE fuse_compat_ts_t3d fuse_compat_ts_t2d)

if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_compat_ts_tests)
else()
    set_target_properties(fuse_compat_ts_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endif()

if(MINGW)
    target_link_options(fuse_compat_ts_tests PRIVATE -Wl,--allow-multiple-definition)
endif()

add_test(NAME fuse_compat_ts COMMAND fuse_compat_ts_tests)
