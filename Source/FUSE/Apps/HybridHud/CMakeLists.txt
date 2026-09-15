add_executable(demo_hybrid_hud main.cpp)

target_link_libraries(demo_hybrid_hud PRIVATE fuse_hybrid fuse_core)
target_compile_features(demo_hybrid_hud PRIVATE cxx_std_17)

if(FUSE_SMOKE_ENABLE_ASAN)
    target_compile_options(demo_hybrid_hud PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(demo_hybrid_hud PRIVATE -fsanitize=address)
endif()
