# WP-4.3 (docs/unification/RENDERER-EXECUTION.md): Intel XeSS Super Resolution plugin ("xess" upscaler).
# Owned by WP-4.3; other packages must not edit this file.
#
#   fuse_xess_plugin        STATIC  loader (runtime-loaded libxess, xess_api.h written from Intel's public docs),
#                                   parameter mapping, XessUpscaler (upscale::ITemporalUpscaler), registration
#                                   (plugins/intel_xess/**). Pure MIT: no Intel header, library or binary.
#   fuse_xess_mock*         MODULE  test-only mock libxess (+ broken variants) driving the CPU gates
#   fuse_xess_layout_check  OBJECT  only with FUSE_XESS_SDK_DIR (developer's own SDK): static_asserts xess_api.h
#                                   against Intel's headers; never in CI
#   fuse_rp_xess_*          ctest   CPU gates (mock runtime; stub-safe); hardware behaviour is manual
#
# FUSE_ENABLE_XESS_PLUGIN (OFF by default and in CI) only controls whether the engine probes for libxess at start-up
# (XessLoader::probe, env FUSE_XESS_SDK_DIR / FUSE_XESS_LIB). The gates never need Intel hardware or software.

option(FUSE_ENABLE_XESS_PLUGIN "Probe for the user's Intel XeSS runtime (libxess) at start-up (OFF in CI)" OFF)
set(FUSE_XESS_SDK_DIR "" CACHE PATH
    "Developer's own XeSS SDK checkout (inc/xess/xess_vk.h): compiles the xess_api.h layout check only")

get_filename_component(_fuse_wp43_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_fuse_wp43_dir "${_fuse_wp43_root}/plugins/intel_xess")

add_library(fuse_xess_plugin STATIC
    ${_fuse_wp43_dir}/src/xess_upscaler.cpp
    ${_fuse_wp43_dir}/include/fuse/renderer/xess/xess_api.h
    ${_fuse_wp43_dir}/include/fuse/renderer/xess/xess_upscaler.hpp)
target_include_directories(fuse_xess_plugin PUBLIC $<BUILD_INTERFACE:${_fuse_wp43_dir}/include>)
target_link_libraries(fuse_xess_plugin PUBLIC fuse_rhi PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(fuse_xess_plugin PRIVATE FUSE_XESS_PLUGIN_ENABLED=$<BOOL:${FUSE_ENABLE_XESS_PLUGIN}>)
fuse_apply_cxx23(fuse_xess_plugin)

if(NOT FUSE_XESS_SDK_DIR STREQUAL "")
    find_package(Vulkan QUIET)
    if(EXISTS "${FUSE_XESS_SDK_DIR}/inc/xess/xess_vk.h" AND Vulkan_FOUND)
        add_library(fuse_xess_layout_check OBJECT ${_fuse_wp43_dir}/src/xess_layout_check.cpp)
        target_include_directories(fuse_xess_layout_check PRIVATE ${_fuse_wp43_dir}/include)
        target_include_directories(fuse_xess_layout_check SYSTEM PRIVATE "${FUSE_XESS_SDK_DIR}/inc" ${Vulkan_INCLUDE_DIRS})
        fuse_apply_cxx23(fuse_xess_layout_check)
        message(STATUS "FUSE: XeSS layout check against ${FUSE_XESS_SDK_DIR}")
    else()
        message(WARNING "FUSE_XESS_SDK_DIR set but inc/xess/xess_vk.h or the Vulkan headers are missing: layout check skipped")
    endif()
endif()

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

# Mock libxess and its broken variants, each in its own directory as "libxess" (the default file name).
set(_fuse_wp43_out "${CMAKE_BINARY_DIR}/fuse_xess_plugin")
set(_fuse_wp43_mocks "")
foreach(_variant IN ITEMS 0 1 2)
    if(_variant EQUAL 0)
        set(_t fuse_xess_mock)
        set(_d mock)
    elseif(_variant EQUAL 1)
        set(_t fuse_xess_mock_oldversion)
        set(_d mock_oldversion)
    else()
        set(_t fuse_xess_mock_noexecute)
        set(_d mock_noexecute)
    endif()
    add_library(${_t} MODULE ${_fuse_wp43_dir}/mock/fuse_xess_mock.cpp)
    target_include_directories(${_t} PRIVATE ${_fuse_wp43_dir}/include ${_fuse_wp43_dir}/mock)
    target_compile_definitions(${_t} PRIVATE FUSE_XESSMOCK_VARIANT=${_variant})
    set_target_properties(${_t} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        PREFIX "lib"
        OUTPUT_NAME "xess"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${_fuse_wp43_out}/${_d}>"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${_fuse_wp43_out}/${_d}>"
        # MSVC writes each module's .exp / .lib next to ARCHIVE_OUTPUT_DIRECTORY; the three mocks share
        # OUTPUT_NAME "xess", so give each its own directory or the links collide (LNK1104 on xess.exp).
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${_fuse_wp43_out}/${_d}>")
    fuse_apply_cxx23(${_t})
    list(APPEND _fuse_wp43_mocks ${_t})
endforeach()

add_executable(fuse_rp_xess_cpu ${_fuse_wp43_root}/tests/test_rp_xess.cpp)
target_include_directories(fuse_rp_xess_cpu PRIVATE ${_fuse_wp43_dir}/mock)
target_link_libraries(fuse_rp_xess_cpu PRIVATE fuse_xess_plugin)
add_dependencies(fuse_rp_xess_cpu ${_fuse_wp43_mocks})
target_compile_definitions(fuse_rp_xess_cpu PRIVATE
    FUSE_XESS_TEST_MOCK="$<TARGET_FILE:fuse_xess_mock>"
    FUSE_XESS_TEST_MOCK_OLDVERSION="$<TARGET_FILE:fuse_xess_mock_oldversion>"
    FUSE_XESS_TEST_MOCK_NOEXECUTE="$<TARGET_FILE:fuse_xess_mock_noexecute>"
    FUSE_XESS_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/xess_scratch")
fuse_apply_cxx23(fuse_rp_xess_cpu)
foreach(_suite discovery marshalling lifecycle registry zero_alloc)
    add_test(NAME fuse_rp_xess_${_suite} COMMAND fuse_rp_xess_cpu ${_suite})
    set_tests_properties(fuse_rp_xess_${_suite} PROPERTIES LABELS "gate;renderer;xess" TIMEOUT 120)
endforeach()

add_test(NAME fuse_rp_xess_no_committed_binaries
    COMMAND "${CMAKE_COMMAND}" -DREPO=${CMAKE_SOURCE_DIR} -P "${_fuse_wp43_dir}/cmake/xess_binary_gate.cmake")
set_tests_properties(fuse_rp_xess_no_committed_binaries PROPERTIES
    LABELS "gate;lint;renderer;xess" TIMEOUT 120 SKIP_REGULAR_EXPRESSION "XESS_BINARY_GATE_SKIP")
