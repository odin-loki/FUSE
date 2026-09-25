# WP-6.4b (docs/unification/RENDERER-EXECUTION.md): denoiser interface + NVIDIA NRD plugin (SIGMA / REBLUR / RELAX).
# Owned by WP-6.4b; other packages must not edit this file.
#
#   fuse_denoiser         STATIC  denoise::IDenoiser, DenoiserRegistry (selection + in-tree fallback) and the
#                                 SvgfDenoiserAdapter over the WP-6.4 SvgfDenoiser (include/fuse/renderer/denoise/
#                                 denoiser.hpp, src/denoise/denoiser.cpp; additive, fuse_denoise is unchanged)
#   fuse_nrd_plugin       STATIC  provider loader (fuse_nrd_plugin_abi.h), FUSE -> NRD mapping, NrdDenoiser
#                                 (IDenoiser, one "denoise.nrd" RG pass), registration (plugins/nvidia_nrd/**). MIT:
#                                 no NRD header, library or binary; the NRD provider is built by the developer.
#   fuse_nrdplugin_mock*  MODULE  test-only mock provider (+ broken variants) driving the CPU gates
#   fuse_rp_nrd_*         ctest   CPU gates (mock provider; stub-safe); hardware behaviour is manual
#
# FUSE_ENABLE_NRD_PLUGIN (OFF by default and in CI) only controls whether the engine probes for a provider at
# start-up (NrdPluginLoader::probe, env FUSE_NRD_SDK_DIR / FUSE_NRD_PLUGIN_LIB / FUSE_NRD_PLUGIN_OPTIONS).

option(FUSE_ENABLE_NRD_PLUGIN "Probe for a developer-built NVIDIA NRD provider at start-up (OFF in CI)" OFF)

get_filename_component(_fuse_wp64b_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_fuse_wp64b_dir "${_fuse_wp64b_root}/plugins/nvidia_nrd")

add_library(fuse_denoiser STATIC
    ${_fuse_wp64b_root}/src/denoise/denoiser.cpp
    ${_fuse_wp64b_root}/include/fuse/renderer/denoise/denoiser.hpp)
target_link_libraries(fuse_denoiser PUBLIC fuse_denoise)
fuse_apply_cxx23(fuse_denoiser)

add_library(fuse_nrd_plugin STATIC
    ${_fuse_wp64b_dir}/src/nrd_denoiser.cpp
    ${_fuse_wp64b_dir}/include/fuse/renderer/nrd/fuse_nrd_plugin_abi.h
    ${_fuse_wp64b_dir}/include/fuse/renderer/nrd/nrd_denoiser.hpp)
target_include_directories(fuse_nrd_plugin PUBLIC $<BUILD_INTERFACE:${_fuse_wp64b_dir}/include>)
target_link_libraries(fuse_nrd_plugin PUBLIC fuse_denoiser PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(fuse_nrd_plugin PRIVATE FUSE_NRD_PLUGIN_ENABLED=$<BOOL:${FUSE_ENABLE_NRD_PLUGIN}>)
fuse_apply_cxx23(fuse_nrd_plugin)

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_fuse_wp64b_out "${CMAKE_BINARY_DIR}/fuse_nrd_plugin")
set(_fuse_wp64b_mocks "")
foreach(_variant IN ITEMS 0 1 2 3)
    if(_variant EQUAL 0)
        set(_t fuse_nrdplugin_mock)
        set(_d mock)
    elseif(_variant EQUAL 1)
        set(_t fuse_nrdplugin_mock_badabi)
        set(_d mock_badabi)
    elseif(_variant EQUAL 2)
        set(_t fuse_nrdplugin_mock_noentry)
        set(_d mock_noentry)
    else()
        set(_t fuse_nrdplugin_mock_truncated)
        set(_d mock_truncated)
    endif()
    add_library(${_t} MODULE ${_fuse_wp64b_dir}/mock/fuse_nrdplugin_mock.cpp)
    target_include_directories(${_t} PRIVATE ${_fuse_wp64b_dir}/include ${_fuse_wp64b_dir}/mock)
    target_compile_definitions(${_t} PRIVATE FUSE_NRDMOCK_VARIANT=${_variant})
    set_target_properties(${_t} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        LIBRARY_OUTPUT_DIRECTORY "$<1:${_fuse_wp64b_out}/${_d}>"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${_fuse_wp64b_out}/${_d}>")
    fuse_apply_cxx23(${_t})
    list(APPEND _fuse_wp64b_mocks ${_t})
endforeach()

add_executable(fuse_rp_nrd_cpu ${_fuse_wp64b_root}/tests/test_rp_nrd.cpp)
target_include_directories(fuse_rp_nrd_cpu PRIVATE ${_fuse_wp64b_dir}/mock)
target_link_libraries(fuse_rp_nrd_cpu PRIVATE fuse_nrd_plugin)
add_dependencies(fuse_rp_nrd_cpu ${_fuse_wp64b_mocks})
target_compile_definitions(fuse_rp_nrd_cpu PRIVATE
    FUSE_NRD_TEST_MOCK="$<TARGET_FILE:fuse_nrdplugin_mock>"
    FUSE_NRD_TEST_MOCK_BADABI="$<TARGET_FILE:fuse_nrdplugin_mock_badabi>"
    FUSE_NRD_TEST_MOCK_NOENTRY="$<TARGET_FILE:fuse_nrdplugin_mock_noentry>"
    FUSE_NRD_TEST_MOCK_TRUNCATED="$<TARGET_FILE:fuse_nrdplugin_mock_truncated>"
    FUSE_NRD_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/nrd_scratch")
fuse_apply_cxx23(fuse_rp_nrd_cpu)
foreach(_suite discovery marshalling graph registry zero_alloc)
    add_test(NAME fuse_rp_nrd_${_suite} COMMAND fuse_rp_nrd_cpu ${_suite})
    set_tests_properties(fuse_rp_nrd_${_suite} PROPERTIES LABELS "gate;renderer;denoise;nrd" TIMEOUT 120)
endforeach()

add_test(NAME fuse_rp_nrd_no_committed_binaries
    COMMAND "${CMAKE_COMMAND}" -DREPO=${CMAKE_SOURCE_DIR} -P "${_fuse_wp64b_dir}/cmake/nrd_binary_gate.cmake")
set_tests_properties(fuse_rp_nrd_no_committed_binaries PROPERTIES
    LABELS "gate;lint;renderer;nrd" TIMEOUT 120 SKIP_REGULAR_EXPRESSION "NRD_BINARY_GATE_SKIP")
