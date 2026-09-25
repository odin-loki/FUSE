# GPU radix sort (Vulkan compute): fuse_rhi sources, build-time SPIR-V and the gate test.
# Master plan B4.11 row "GPU radix sort produces correctly sorted (key, value) pairs — verified
# with reference CPU sort". Owned by one work stream; other topics must not edit this file.
#
# Shaders: shaders/compute/radix_*.comp -> ${binary}/shaders/compute/*.comp.spv with
# glslangValidator -V (+ spirv-val when present), same convention as b5_rhi_rows.cmake. The
# histogram/scatter kernels are compiled twice: 32-bit keys and -DFUSE_RADIX_KEY64 (u64 keys).

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/compute/gpu_radix_sort.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/compute/gpu_radix_sort.cpp
)

set(FUSE_RADIX_SORT_SHADER_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../shaders/compute")
set(FUSE_RADIX_SORT_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/compute")
if(FUSE_GLSLANG_VALIDATOR)
    file(MAKE_DIRECTORY "${FUSE_RADIX_SORT_SHADER_OUT_DIR}")
    set(_fuse_radix_spv_outputs "")
    # <output name>|<source>|<extra define or empty>
    foreach(_entry
            "radix_histogram|radix_histogram.comp|"
            "radix_scan|radix_scan.comp|"
            "radix_scan_add|radix_scan_add.comp|"
            "radix_scatter|radix_scatter.comp|"
            "radix_histogram_k64|radix_histogram.comp|-DFUSE_RADIX_KEY64"
            "radix_scatter_k64|radix_scatter.comp|-DFUSE_RADIX_KEY64")
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _source)
        list(LENGTH _parts _part_count)
        set(_define "")
        if(_part_count GREATER 2)
            list(GET _parts 2 _define)
        endif()
        set(_src "${FUSE_RADIX_SORT_SHADER_SRC_DIR}/${_source}")
        set(_out "${FUSE_RADIX_SORT_SHADER_OUT_DIR}/${_name}.comp.spv")
        set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V ${_define} "${_src}" -o "${_out}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" "${_out}")
        endif()
        add_custom_command(OUTPUT "${_out}"
            ${_cmds}
            DEPENDS "${_src}" "${FUSE_RADIX_SORT_SHADER_SRC_DIR}/radix_sort_common.glsl"
            COMMENT "glslangValidator ${_source} ${_define} -> ${_name}.comp.spv"
            VERBATIM)
        list(APPEND _fuse_radix_spv_outputs "${_out}")
    endforeach()
    add_custom_target(fuse_radix_sort_shaders ALL DEPENDS ${_fuse_radix_spv_outputs})
    add_dependencies(fuse_rhi fuse_radix_sort_shaders)
    target_compile_definitions(fuse_rhi PRIVATE FUSE_RADIX_SORT_SHADER_DIR="${FUSE_RADIX_SORT_SHADER_OUT_DIR}")
    message(STATUS "FUSE: GPU radix sort shaders compiled at build time (${FUSE_RADIX_SORT_SHADER_OUT_DIR})")
else()
    message(STATUS "FUSE: glslangValidator not found — GPU radix sort unavailable (gate skips)")
endif()

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_radix_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
    add_executable(fuse_gpu_radix_sort_gates
        ${CMAKE_CURRENT_LIST_DIR}/../tests/test_gpu_radix_sort_gates.cpp)
    target_link_libraries(fuse_gpu_radix_sort_gates PRIVATE fuse_rhi fuse_physics fuse_core)
    if(TARGET fuse_radix_sort_shaders)
        target_compile_definitions(fuse_gpu_radix_sort_gates PRIVATE FUSE_RADIX_SORT_SHADERS_BUILT=1)
    endif()
    add_test(NAME fuse_gpu_radix_sort_gates
             COMMAND "${_fuse_radix_lock}" "$<TARGET_FILE:fuse_gpu_radix_sort_gates>")
    set_tests_properties(fuse_gpu_radix_sort_gates PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 1500
        LABELS "gate;vulkan")
endif()
