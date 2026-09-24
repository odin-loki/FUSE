# FUSE Relight RL-1.1: tap seam + device import (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.2, §2.4).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(tap)
#
# Targets
#   fuse_relight_tap        IRelightTap, NullTap, RecordingTap, tap/device-import options
#                           (RL-0.6 options). Any platform; the CPU unit tests build on Linux too.
#   fuse_relight_tap_vk     (PE builds) plain-Vulkan bootstrap of the FUSE-owned VkInstance/VkDevice
#                           that DXVK imports (FUSE-DXVK patches RL-1.1-01/02). Link-only
#                           dependency of relight_dxvk, so d3d9.dll and d3d8.dll both carry it.
#   fuse_relight_tap_d3d9   (PE builds) the dispatcher inside d3d9.dll (dxvk/fuse_tap_dxvk.cpp):
#                           DXVK state -> IRelightTap events. Compiled against DXVK's private d3d9
#                           headers as system headers and linked into relight_d3d9. It creates the
#                           tap with createTapForDevice (device_tap.hpp), which capture/ defines.
#   fuse_relight_tap_capture  (capture/CMakeLists.txt, added after RL-1.2/1.3/1.4) the capture mode.
# Tests: ../tests/tap (rl_tap_unit; Wine: rl_passthrough_golden_*, rl_tap_record_*, rl_tap_validation).

add_library(fuse_relight_tap STATIC
    src/recording_tap.cpp
    src/sha256.cpp
    src/tap_config.cpp
)
target_include_directories(fuse_relight_tap
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fuse_relight_tap PUBLIC fuse_relight_options)
target_compile_features(fuse_relight_tap PUBLIC cxx_std_17)

if(TARGET relight_d3d9)
    # ---- Vulkan bootstrap (DXVK's vendored Vulkan headers: the struct set DXVK 3.1.1 uses) --------
    add_library(fuse_relight_tap_vk STATIC src/vk_bootstrap.cpp)
    target_include_directories(fuse_relight_tap_vk SYSTEM PRIVATE "${FUSE_DXVK_DIR}/include/vulkan/include")
    target_link_libraries(fuse_relight_tap_vk PUBLIC fuse_relight_tap)
    set_target_properties(fuse_relight_tap_vk PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    # DXVK's instance/adapter code calls into it (patches RL-1.1-01/02); link only, so FUSE's usage
    # requirements (fuse_core include dirs and defines) do not reach the vendored sources.
    target_link_libraries(relight_dxvk PUBLIC $<LINK_ONLY:fuse_relight_tap_vk>)

    # ---- d3d9 dispatcher ----------------------------------------------------------------------------
    add_library(fuse_relight_tap_d3d9 OBJECT dxvk/fuse_tap_dxvk.cpp)
    set_target_properties(fuse_relight_tap_d3d9 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(fuse_relight_tap_d3d9 SYSTEM PRIVATE
        "${FUSE_DXVK_DIR}/src/d3d9"
        "${CMAKE_BINARY_DIR}/relight/dxvk-gen/d3d9")
    target_include_directories(fuse_relight_tap_d3d9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dxvk)
    target_link_libraries(fuse_relight_tap_d3d9 PRIVATE relight_dxvk fuse_relight_tap)
    add_dependencies(fuse_relight_tap_d3d9 fuse_relight_tap_vk)

    target_sources(relight_d3d9 PRIVATE $<TARGET_OBJECTS:fuse_relight_tap_d3d9>)
    # d3d9_device.h includes fuse_tap_dxvk.h (patch RL-1.1-03).
    target_include_directories(relight_d3d9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dxvk)
    target_link_libraries(relight_d3d9 PRIVATE $<LINK_ONLY:fuse_relight_tap>)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests/tap ${CMAKE_CURRENT_BINARY_DIR}/tests)
