add_library(fuse_fx STATIC
    src/cast_pipeline.cpp
    src/effect_descriptor.cpp
    src/fx_composer.cpp
    src/residual_effects.cpp
    src/spell_descriptor.cpp
    include/fuse/fx/cast_pipeline.hpp
    include/fuse/fx/effect_descriptor.hpp
    include/fuse/fx/fx_composer.hpp
    include/fuse/fx/fx_defs.hpp
    include/fuse/fx/residual_effects.hpp
    include/fuse/fx/spell_descriptor.hpp
)

add_library(fuse::fx ALIAS fuse_fx)

target_include_directories(fuse_fx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_fx PUBLIC fuse_core fuse_world2d fuse_world3d)
target_compile_features(fuse_fx PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
