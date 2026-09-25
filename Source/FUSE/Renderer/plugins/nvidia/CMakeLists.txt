# Optional NVIDIA plugin seam (docs/nvidia-plugin.md). Nothing here links NVIDIA code:
#
#   fuse_nvidia_plugin              loader + input contract (always built; pure MIT, no NVIDIA headers)
#   fuse_nvidia_streamline_mapping  FUSE ABI -> Streamline types (vendored MIT headers, Engine/lib/streamline)
#   fuse_nvplugin_streamline        MODULE provider: loads sl.interposer at runtime (MIT)
#   fuse_nvplugin_ngx               MODULE Linux bridge against the developer's own DLSS SDK — only with
#                                   FUSE_ENABLE_NVIDIA_PLUGIN=ON and FUSE_NVIDIA_DLSS_SDK_DIR set; its
#                                   binary links NVIDIA's static NGX loader and must never be committed
#   fuse_nvplugin_mock*, fuse_sl_mock_interposer   test-only MIT mocks driving the CI gates
#
# FUSE_ENABLE_NVIDIA_PLUGIN (OFF by default and in every CI preset) only controls whether the engine
# probes for an NVIDIA runtime at start-up (NvPluginLoader::probe) and whether the NGX bridge is built.
# The mock gates below never need NVIDIA hardware, SDKs or binaries and run in every test build.

option(FUSE_ENABLE_NVIDIA_PLUGIN
    "Probe for the optional NVIDIA DLSS/Streamline runtime at start-up (dev machines with RTX GPUs; OFF in CI)" OFF)
set(FUSE_NVIDIA_DLSS_SDK_DIR "" CACHE PATH
    "Linux only: root of the developer's NVIDIA DLSS SDK checkout (include/, lib/Linux_x86_64/) for fuse_nvplugin_ngx")

set(FUSE_STREAMLINE_DIR "${FUSE_VENDOR_DIR}/streamline")
set(FUSE_NVIDIA_PLUGIN_OUT "${CMAKE_BINARY_DIR}/fuse_nvidia_plugin")

# The provider C ABI alone (header-only; what providers and mocks compile against — no fuse_core).
add_library(fuse_nvidia_plugin_abi INTERFACE)
target_include_directories(fuse_nvidia_plugin_abi INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

add_library(fuse_nvidia_plugin STATIC
    src/nv_plugin_loader.cpp
    src/nv_input_mapping.cpp
    include/fuse/renderer/nvidia/fuse_nv_plugin_abi.h
    include/fuse/renderer/nvidia/nv_plugin_loader.hpp
    include/fuse/renderer/nvidia/nv_input_mapping.hpp
)
target_include_directories(fuse_nvidia_plugin PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(fuse_nvidia_plugin PUBLIC fuse_nvidia_plugin_abi fuse_core PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(fuse_nvidia_plugin PRIVATE
    FUSE_NVIDIA_PLUGIN_ENABLED=$<BOOL:${FUSE_ENABLE_NVIDIA_PLUGIN}>)
if(FUSE_ENABLE_NVIDIA_PLUGIN)
    message(STATUS "FUSE: NVIDIA plugin probe ON (FUSE_NVIDIA_SDK_DIR at runtime; docs/nvidia-plugin.md)")
endif()

# Streamline type mapping: compiled everywhere so CI checks it against the pinned headers.
add_library(fuse_nvidia_streamline_mapping STATIC streamline/nv_streamline_mapping.cpp streamline/nv_streamline_mapping.hpp)
target_include_directories(fuse_nvidia_streamline_mapping PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/streamline")
# SYSTEM: third-party headers stay out of FUSE's -Wall -Wextra -Werror.
target_include_directories(fuse_nvidia_streamline_mapping SYSTEM PUBLIC "${FUSE_STREAMLINE_DIR}/include")
target_link_libraries(fuse_nvidia_streamline_mapping PUBLIC fuse_nvidia_plugin_abi)
# Linked into the MODULE provider, so position-independent even in static-library form.
set_target_properties(fuse_nvidia_streamline_mapping PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FUSE_STREAMLINE_DIR}/VERSION")

function(_fuse_nvidia_module target out_dir)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        # $<1:...> keeps multi-config generators from appending a per-config subdirectory.
        LIBRARY_OUTPUT_DIRECTORY "$<1:${out_dir}>"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${out_dir}>")
endfunction()

# Streamline provider (the Windows production path; also built here so CI drives it through the mock
# interposer). Windows file name: fuse_nvplugin_streamline.dll (NvPluginLoader's default).
add_library(fuse_nvplugin_streamline MODULE providers/streamline/fuse_nvplugin_streamline.cpp)
target_link_libraries(fuse_nvplugin_streamline PRIVATE fuse_nvidia_streamline_mapping ${CMAKE_DL_LIBS})
_fuse_nvidia_module(fuse_nvplugin_streamline "${FUSE_NVIDIA_PLUGIN_OUT}/streamline")

# Linux NGX bridge: needs the developer's DLSS SDK (RTX SDKs License; headers not vendored).
if(FUSE_ENABLE_NVIDIA_PLUGIN AND NOT WIN32 AND NOT FUSE_NVIDIA_DLSS_SDK_DIR STREQUAL "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/providers/ngx/fuse_nvplugin_ngx.cpp")
        include("${CMAKE_CURRENT_SOURCE_DIR}/providers/ngx/ngx_bridge.cmake")
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()

# Registration into the upscaler registry + look-graph placement needs the upscaler abstraction
# (cmake/upscale.cmake) and the Look system (cmake/look.cmake) compiled into fuse_rhi. Their fragments
# are included by Source/FUSE/Renderer/CMakeLists.txt in any order relative to this directory, so the
# check runs at the end of the Renderer directory (before the top-level warning/sanitizer finalize).
set(_FUSE_NVIDIA_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
function(_fuse_nvidia_wire_backends)
    set(_dir "${_FUSE_NVIDIA_PLUGIN_DIR}")
    get_target_property(_srcs fuse_rhi SOURCES)
    set(_has_upscale FALSE)
    set(_has_look FALSE)
    foreach(_s IN LISTS _srcs)
        if(_s MATCHES "upscale/upscaler_registry\\.cpp$")
            set(_has_upscale TRUE)
        elseif(_s MATCHES "look/look_params\\.cpp$")
            set(_has_look TRUE)
        endif()
    endforeach()
    if(NOT _has_upscale OR NOT _has_look)
        message(STATUS "FUSE: NVIDIA backend registration (fuse_nvidia_backends) skipped: fuse_rhi lacks "
                       "upscale/upscaler_registry.cpp or look/look_params.cpp")
        return()
    endif()
    add_library(fuse_nvidia_backends STATIC
        "${_dir}/src/nv_backends.cpp"
        "${_dir}/include/fuse/renderer/nvidia/nv_backends.hpp")
    target_link_libraries(fuse_nvidia_backends PUBLIC fuse_nvidia_plugin fuse_rhi)
    set_target_properties(fuse_nvidia_backends PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(FUSE_BUILD_CORE_TESTS)
        add_executable(fuse_nvidia_backend_gates "${_dir}/tests/test_nvidia_backend_gates.cpp")
        target_include_directories(fuse_nvidia_backend_gates PRIVATE "${_dir}/mock")
        target_link_libraries(fuse_nvidia_backend_gates PRIVATE fuse_nvidia_backends)
        set_target_properties(fuse_nvidia_backend_gates PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        add_dependencies(fuse_nvidia_backend_gates fuse_nvplugin_mock)
        target_compile_definitions(fuse_nvidia_backend_gates PRIVATE
            FUSE_NV_TEST_MOCK="$<TARGET_FILE:fuse_nvplugin_mock>")
        add_test(NAME fuse_nvidia_backend_gates COMMAND fuse_nvidia_backend_gates)
        set_tests_properties(fuse_nvidia_backend_gates PROPERTIES LABELS "gate;nvidia" TIMEOUT 120)
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.." CALL _fuse_nvidia_wire_backends)
