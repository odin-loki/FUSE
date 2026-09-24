# NVIDIA plugin seam gates: mock providers + mock Streamline interposer (MIT, no NVIDIA code).

set(_nv_mock_src "${CMAKE_CURRENT_SOURCE_DIR}/../mock")

# Mock provider and its broken variants, each in its own directory so a test can point
# FUSE_NVIDIA_SDK_DIR at exactly one of them.
foreach(_variant IN ITEMS 0 1 2 3)
    if(_variant EQUAL 0)
        set(_t fuse_nvplugin_mock)
        set(_dir mock)
    elseif(_variant EQUAL 1)
        set(_t fuse_nvplugin_mock_badabi)
        set(_dir mock_badabi)
    elseif(_variant EQUAL 2)
        set(_t fuse_nvplugin_mock_noentry)
        set(_dir mock_noentry)
    else()
        set(_t fuse_nvplugin_mock_truncated)
        set(_dir mock_truncated)
    endif()
    add_library(${_t} MODULE "${_nv_mock_src}/fuse_nvplugin_mock.cpp")
    target_include_directories(${_t} PRIVATE "${_nv_mock_src}")
    target_link_libraries(${_t} PRIVATE fuse_nvidia_plugin_abi)
    target_compile_definitions(${_t} PRIVATE FUSE_NVMOCK_VARIANT=${_variant})
    _fuse_nvidia_module(${_t} "${FUSE_NVIDIA_PLUGIN_OUT}/${_dir}")
    list(APPEND _nv_mock_targets ${_t})
endforeach()

# Mock interposer next to the real Streamline provider's test copy.
add_library(fuse_sl_mock_interposer MODULE "${_nv_mock_src}/fuse_sl_mock_interposer.cpp")
target_include_directories(fuse_sl_mock_interposer PRIVATE "${_nv_mock_src}")
target_include_directories(fuse_sl_mock_interposer SYSTEM PRIVATE "${FUSE_STREAMLINE_DIR}/include")
target_compile_definitions(fuse_sl_mock_interposer PRIVATE $<$<PLATFORM_ID:Windows>:SL_INTERPOSER>)
_fuse_nvidia_module(fuse_sl_mock_interposer "${FUSE_NVIDIA_PLUGIN_OUT}/streamline")

add_executable(fuse_nvidia_plugin_gates test_nvidia_plugin_gates.cpp)
target_include_directories(fuse_nvidia_plugin_gates PRIVATE "${_nv_mock_src}")
target_link_libraries(fuse_nvidia_plugin_gates PRIVATE fuse_nvidia_plugin fuse_nvidia_streamline_mapping)
add_dependencies(fuse_nvidia_plugin_gates ${_nv_mock_targets} fuse_sl_mock_interposer fuse_nvplugin_streamline)
target_compile_definitions(fuse_nvidia_plugin_gates PRIVATE
    FUSE_NV_TEST_MOCK="$<TARGET_FILE:fuse_nvplugin_mock>"
    FUSE_NV_TEST_MOCK_BADABI="$<TARGET_FILE:fuse_nvplugin_mock_badabi>"
    FUSE_NV_TEST_MOCK_NOENTRY="$<TARGET_FILE:fuse_nvplugin_mock_noentry>"
    FUSE_NV_TEST_MOCK_TRUNCATED="$<TARGET_FILE:fuse_nvplugin_mock_truncated>"
    FUSE_NV_TEST_SL_PROVIDER="$<TARGET_FILE:fuse_nvplugin_streamline>"
    FUSE_NV_TEST_SL_INTERPOSER="$<TARGET_FILE_NAME:fuse_sl_mock_interposer>"
    FUSE_NV_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")
add_test(NAME fuse_nvidia_plugin_gates COMMAND fuse_nvidia_plugin_gates)
set_tests_properties(fuse_nvidia_plugin_gates PROPERTIES LABELS "gate;nvidia" TIMEOUT 120)

# Licence gate: no NVIDIA RTX SDK binary may be committed, and .gitignore must keep them out.
add_test(NAME fuse_nvidia_no_committed_binaries
    COMMAND "${CMAKE_COMMAND}" -DREPO=${CMAKE_SOURCE_DIR} -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nvidia_binary_gate.cmake")
set_tests_properties(fuse_nvidia_no_committed_binaries PROPERTIES
    LABELS "gate;lint;nvidia" TIMEOUT 120 SKIP_REGULAR_EXPRESSION "NVIDIA_BINARY_GATE_SKIP")
