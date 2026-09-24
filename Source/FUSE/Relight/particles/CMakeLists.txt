# FUSE Relight RL-3.6: the particle system (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.7, §5.8, Wave R3).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(particles), after RL-3.4 (replace).
#
# Targets
#   fuse_relight_particles       the simulation, pure CPU (native and MinGW): descriptions (schema fallbacks, global
#                                preset, animation tables), curve baking, the single-source kernels (spawn, evolve,
#                                billboard; kernel::Backend::CpuReference / CpuParallel), the manager (systems, rings,
#                                conservative counts, spawn contexts, emitter-geometry pool) and the CPU backend, the
#                                rtx.particles.* options.
#   fuse_relight_particles_mods  USD ParticleSystemAPI / RL-3.2 relight_particles records -> descriptions (curve baking),
#                                the catalog of a ReplacementEngine's mods (particle records, emitter meshes) and the
#                                emitter bridge (spawning from replaced parts, kept originals and ParticleEmitter draws).
#   fuse_relight_particles_vk    the Vulkan compute backend (renderer Vulkan backend only): the GLSL twin
#                                (shaders/particle_sim.comp, three passes) compiled with glslangValidator, validated with
#                                spirv-val and embedded.
# Tests: tests/ (rl_particles_*).

foreach(_dep fuse_relight_replace fuse_relight_mods_usd fuse_relight_mods_import fuse_relight_options fuse_relight_hash)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "particles needs ${_dep} (add_subdirectory(particles) after RL-3.4 replace)")
    endif()
endforeach()

macro(_rl_particles_cxx23 _t)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endmacro()

add_library(fuse_relight_particles STATIC
    src/curve_bake.cpp
    src/particle_desc.cpp
    src/particle_options.cpp
    src/particle_system.cpp)
target_include_directories(fuse_relight_particles PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_particles PUBLIC fuse_core fuse_relight_options PRIVATE fuse_relight_hash)
_rl_particles_cxx23(fuse_relight_particles)

add_library(fuse_relight_particles_mods STATIC src/particle_mods.cpp)
target_link_libraries(fuse_relight_particles_mods
    PUBLIC fuse_relight_particles fuse_relight_replace fuse_relight_mods_usd fuse_relight_capture_export
    PRIVATE fuse_relight_mods_import fuse_relight_hash)
_rl_particles_cxx23(fuse_relight_particles_mods)

set(RL_PARTICLES_VK OFF)
if(FUSE_VULKAN_BACKEND AND TARGET fuse_rhi AND FUSE_GLSLANG_VALIDATOR)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
    file(MAKE_DIRECTORY "${_gen}" "${_spv}")
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle_sim.comp")
    set(_common "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle_common.glsl")
    set(_outs "")
    foreach(_pass 0 1 2)
        set(_out "${_spv}/particle_pass${_pass}.spv")
        set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 -DPARTICLE_PASS=${_pass} "${_src}" -o "${_out}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
        endif()
        add_custom_command(OUTPUT "${_out}" ${_cmds} DEPENDS "${_src}" "${_common}"
            COMMENT "glslangValidator particle_sim.comp (PARTICLE_PASS=${_pass})" VERBATIM)
        list(APPEND _outs "${_out}")
    endforeach()
    list(GET _outs 0 _spawn)
    list(GET _outs 1 _evolve)
    list(GET _outs 2 _billboard)
    set(_header "${_gen}/relight_particle_spv.h")
    add_custom_command(OUTPUT "${_header}"
        COMMAND "${CMAKE_COMMAND}" "-DOUT=${_header}" "-DSPAWN=${_spawn}" "-DEVOLVE=${_evolve}" "-DBILLBOARD=${_billboard}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
        DEPENDS ${_outs} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
        COMMENT "Embedding the particle kernels' SPIR-V" VERBATIM)
    add_custom_target(fuse_relight_particles_spv DEPENDS "${_header}")
    add_library(fuse_relight_particles_vk STATIC src/particle_vulkan.cpp)
    add_dependencies(fuse_relight_particles_vk fuse_relight_particles_spv)
    target_include_directories(fuse_relight_particles_vk PRIVATE "${_gen}")
    target_link_libraries(fuse_relight_particles_vk PUBLIC fuse_relight_particles fuse_rhi)
    _rl_particles_cxx23(fuse_relight_particles_vk)
    set(RL_PARTICLES_VK ON)
endif()

add_subdirectory(tests)
