add_library(fuse_fx STATIC
    src/cast_pipeline.cpp
    src/effect_descriptor.cpp
    src/effect_graph.cpp
    src/effect_timeline.cpp
    src/afx_template_pack.cpp
    src/afx_mission_hooks.cpp
    src/afx_mission_loader.cpp
    src/afx_mission_script_vm.cpp
    src/fx_composer.cpp
    src/particle_pool.cpp
    src/particle_pool_gpu.cpp
    src/afx_choreographer_bridge.cpp
    src/parameter_bind.cpp
    src/missile_descriptor.cpp
    src/socket_constraint.cpp
    src/residual_effects.cpp
    src/spell_descriptor.cpp
    include/fuse/fx/cast_pipeline.hpp
    include/fuse/fx/effect_descriptor.hpp
    include/fuse/fx/effect_graph.hpp
    include/fuse/fx/effect_timeline.hpp
    include/fuse/fx/afx_template_pack.hpp
    include/fuse/fx/afx_mission_hooks.hpp
    include/fuse/fx/afx_mission_loader.hpp
    include/fuse/fx/afx_mission_script_vm.hpp
    include/fuse/fx/fx_composer.hpp
    include/fuse/fx/fx_defs.hpp
    include/fuse/fx/fx_socket.hpp
    include/fuse/fx/particle_pool.hpp
    include/fuse/fx/particle_pool_gpu.hpp
    include/fuse/fx/particle_pool_kernel.hpp
    include/fuse/fx/afx_choreographer_bridge.hpp
    include/fuse/fx/parameter_bind.hpp
    include/fuse/fx/missile_descriptor.hpp
    include/fuse/fx/socket_constraint.hpp
    include/fuse/fx/residual_effects.hpp
    include/fuse/fx/spell_descriptor.hpp
)

add_library(fuse::fx ALIAS fuse_fx)

target_include_directories(fuse_fx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_fx PUBLIC fuse_core fuse_world2d fuse_world3d)

if(FUSE_CUDA_BACKEND)
    target_sources(fuse_fx PRIVATE src/particle_pool_cuda.cu)
    target_compile_definitions(fuse_fx PUBLIC FUSE_HAS_CUDA=1)
    target_link_libraries(fuse_fx PUBLIC CUDA::cudart)
    set_target_properties(fuse_fx PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
