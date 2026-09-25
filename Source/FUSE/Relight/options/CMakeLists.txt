# FUSE Relight RL-0.6: options system (rtx.conf-compatible typed, layered options).
# Ported from dxvk-remix rtx_option* (MIT); see the per-file notices.

add_library(fuse_relight_options STATIC
    src/hash_set_layer.cpp
    src/option.cpp
    src/option_config.cpp
    src/option_layer.cpp
    src/option_manager.cpp
    src/option_value.cpp
)
target_include_directories(fuse_relight_options PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_options PUBLIC fuse_core)
target_compile_features(fuse_relight_options PUBLIC cxx_std_17)
