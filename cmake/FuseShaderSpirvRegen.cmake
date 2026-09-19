# Optional configure-time / target SPIR-V regeneration when glslangValidator is available.
# CI runners with glslang-tools installed regenerate composite.frag.spv automatically.

find_program(FUSE_GLSLANG_VALIDATOR NAMES glslangValidator)

if(FUSE_GLSLANG_VALIDATOR)
    set(FUSE_COMPOSITE_FRAG_SRC "${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures/composite.frag")
    set(FUSE_COMPOSITE_FRAG_SPV "${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures/composite.frag.spv")

    message(STATUS "FUSE: glslangValidator found — composite.frag.spv regen enabled")

    add_custom_target(fuse_regen_shader_fixtures
        COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V "${FUSE_COMPOSITE_FRAG_SRC}" -o "${FUSE_COMPOSITE_FRAG_SPV}"
        BYPRODUCTS "${FUSE_COMPOSITE_FRAG_SPV}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Regenerate composite.frag.spv from composite.frag"
        VERBATIM)

    if(EXISTS "${FUSE_COMPOSITE_FRAG_SRC}")
        file(TIMESTAMP "${FUSE_COMPOSITE_FRAG_SRC}" _fuse_composite_frag_src_time)
        if(EXISTS "${FUSE_COMPOSITE_FRAG_SPV}")
            file(TIMESTAMP "${FUSE_COMPOSITE_FRAG_SPV}" _fuse_composite_frag_spv_time)
        else()
            set(_fuse_composite_frag_spv_time "0")
        endif()

        if(_fuse_composite_frag_src_time GREATER _fuse_composite_frag_spv_time)
            execute_process(
                COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V "${FUSE_COMPOSITE_FRAG_SRC}"
                        -o "${FUSE_COMPOSITE_FRAG_SPV}"
                RESULT_VARIABLE _fuse_composite_regen_result
                OUTPUT_VARIABLE _fuse_composite_regen_out
                ERROR_VARIABLE _fuse_composite_regen_err
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_STRIP_TRAILING_WHITESPACE)
            if(_fuse_composite_regen_result EQUAL 0)
                message(STATUS "FUSE: regenerated composite.frag.spv at configure time")
            else()
                message(STATUS "FUSE: composite.frag.spv configure-time regen skipped: ${_fuse_composite_regen_err}")
            endif()
        endif()
    endif()
else()
    message(STATUS "FUSE: glslangValidator not found — using checked-in composite.frag.spv fixture")
endif()
