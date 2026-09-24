# WP-0.4 (docs/unification/RENDERER-EXECUTION.md): bindless heap hardening — 32-bit shader handles
# (shaders/common/bindless.glsl), buffer-address table, sampler heap, deferred (fence/timeline
# tied) slot release, and the VK_EXT_descriptor_buffer backend (WP-0.4b) with the
# descriptor-indexing fallback. Owned by WP-0.4; other packages must not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/bindless_internal.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/bindless_descriptor_buffer.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_rp_wp04_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")

    add_executable(fuse_rp_bindless tests/test_rp_bindless.cpp)
    target_link_libraries(fuse_rp_bindless PRIVATE fuse_rhi)
    if(FUSE_GLSLANG_VALIDATOR)
        set(_fuse_bl_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/bindless")
        file(MAKE_DIRECTORY "${_fuse_bl_dir}")
        set(_fuse_bl_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bindless/rp_bindless_test.comp")
        set(_fuse_bl_inc "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common")
        set(_fuse_bl_spv "${_fuse_bl_dir}/rp_bindless_test.comp.spv")
        set(_fuse_bl_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 "-I${_fuse_bl_inc}"
                          "${_fuse_bl_src}" -o "${_fuse_bl_spv}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _fuse_bl_cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_fuse_bl_spv}")
        endif()
        add_custom_command(OUTPUT "${_fuse_bl_spv}" ${_fuse_bl_cmds}
            DEPENDS "${_fuse_bl_src}" "${_fuse_bl_inc}/bindless.glsl"
            COMMENT "glslangValidator rp_bindless_test.comp -> rp_bindless_test.comp.spv"
            VERBATIM)
        add_custom_target(fuse_rp_bindless_shaders DEPENDS "${_fuse_bl_spv}")
        add_dependencies(fuse_rp_bindless fuse_rp_bindless_shaders)
        target_compile_definitions(fuse_rp_bindless PRIVATE FUSE_RP_BINDLESS_SHADER="${_fuse_bl_spv}")
    endif()

    # Exit test: a shader indexes 10k textures + a BDA buffer (+ sampler heap, UBO, storage image)
    # through 32-bit handles; readback matches. Same test on both backends and on the fallback.
    # Churn: deferred release tied to frame fences, 2 frames in flight. Exit 77 = skip.
    set(_fuse_bl_tests "")
    foreach(_mode textures churn)
        foreach(_backend set buffer fallback)
            set(_name "fuse_rp_bindless_${_mode}_${_backend}")
            add_test(NAME ${_name}
                     COMMAND "${_fuse_rp_wp04_lock}" "$<TARGET_FILE:fuse_rp_bindless>" --mode ${_mode} --backend ${_backend})
            list(APPEND _fuse_bl_tests ${_name})
        endforeach()
    endforeach()
    set_tests_properties(${_fuse_bl_tests} PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        LABELS "gate;vulkan;renderer;bindless")
endif()
