# B2 gate row: "Shader compiler produces valid SPIR-V for all test shaders — verified with spirv-val".
#
#   cmake -DFUSE_SPIRV_VAL=<spirv-val> -DFUSE_GLSLANG_VALIDATOR=<glslangValidator or empty>
#         -DFUSE_SHADER_DIRS=<dir|dir> -DFUSE_GLSL_SOURCE_DIRS=<dir|dir>
#         -DFUSE_GLSL_INCLUDE_DIR=<dir> -DFUSE_WORK_DIR=<dir> -P b5_rhi_spirv_val.cmake
#
# 1. Every checked-in / build-generated `.spv` under FUSE_SHADER_DIRS must pass spirv-val.
# 2. When glslangValidator is available, every GLSL source (.vert/.frag/.comp) under
#    FUSE_GLSL_SOURCE_DIRS is compiled fresh and the output must pass spirv-val too, so a shader
#    whose checked-in .spv went stale (or has none) is still covered.
# Exit 77 (SKIP_RETURN_CODE) when spirv-val is not installed.

if(NOT FUSE_SPIRV_VAL OR NOT EXISTS "${FUSE_SPIRV_VAL}")
    message("SKIP: spirv-val not installed")
    cmake_language(EXIT 77)
endif()

string(REPLACE "|" ";" _shader_dirs "${FUSE_SHADER_DIRS}")
string(REPLACE "|" ";" _source_dirs "${FUSE_GLSL_SOURCE_DIRS}")

set(_checked 0)
set(_failed "")

function(_fuse_spirv_validate spv label)
    execute_process(COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.2 "${spv}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message("spirv-val FAILED: ${label}\n${_out}${_err}")
        set(_failed "${_failed};${label}" PARENT_SCOPE)
    else()
        message("spirv-val ok: ${label}")
    endif()
endfunction()

foreach(_dir IN LISTS _shader_dirs)
    if(NOT _dir OR NOT IS_DIRECTORY "${_dir}")
        continue()
    endif()
    file(GLOB_RECURSE _spvs "${_dir}/*.spv")
    foreach(_spv IN LISTS _spvs)
        _fuse_spirv_validate("${_spv}" "${_spv}")
        math(EXPR _checked "${_checked} + 1")
    endforeach()
endforeach()

set(_compiled 0)
if(FUSE_GLSLANG_VALIDATOR AND EXISTS "${FUSE_GLSLANG_VALIDATOR}")
    file(MAKE_DIRECTORY "${FUSE_WORK_DIR}")
    foreach(_dir IN LISTS _source_dirs)
        if(NOT _dir OR NOT IS_DIRECTORY "${_dir}")
            continue()
        endif()
        file(GLOB _sources "${_dir}/*.vert" "${_dir}/*.frag" "${_dir}/*.comp")
        foreach(_src IN LISTS _sources)
            get_filename_component(_name "${_src}" NAME)
            set(_out "${FUSE_WORK_DIR}/${_name}.spv")
            execute_process(
                COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V "-I${FUSE_GLSL_INCLUDE_DIR}" "${_src}" -o "${_out}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out_text ERROR_VARIABLE _err_text)
            if(NOT _rc EQUAL 0)
                message("glslangValidator FAILED: ${_src}\n${_out_text}${_err_text}")
                list(APPEND _failed "${_src} (compile)")
                continue()
            endif()
            _fuse_spirv_validate("${_out}" "${_src} (fresh compile)")
            math(EXPR _compiled "${_compiled} + 1")
        endforeach()
    endforeach()
endif()

message("spirv-val: ${_checked} shipped/build .spv files, ${_compiled} fresh GLSL compiles")
if(_checked EQUAL 0)
    message(FATAL_ERROR "no .spv files found under ${FUSE_SHADER_DIRS}")
endif()
list(REMOVE_ITEM _failed "")
list(LENGTH _failed _failed_count)
if(_failed_count GREATER 0)
    message(FATAL_ERROR "invalid SPIR-V: ${_failed}")
endif()
