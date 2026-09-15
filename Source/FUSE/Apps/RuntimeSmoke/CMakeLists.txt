add_executable(fuse_runtime_smoke
    main.cpp
)

target_link_libraries(fuse_runtime_smoke
    PRIVATE
        fuse_core
        fuse_t3d_legacy
        fuse_t2d_legacy
)

target_compile_features(fuse_runtime_smoke PRIVATE cxx_std_17)

if(FUSE_SMOKE_ENABLE_ASAN)
    target_compile_options(fuse_runtime_smoke PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fuse_runtime_smoke PRIVATE -fsanitize=address)
endif()

add_test(NAME fuse_runtime_smoke COMMAND fuse_runtime_smoke)
