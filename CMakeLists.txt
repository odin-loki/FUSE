cmake_minimum_required(VERSION 3.21.0)

# -----------------------------------------------------------------------------
# FUSE umbrella entry (U1) — wraps Torque3D, optional Torque2D, and fuse_core.
#
# Legacy T3D-only path (unchanged workflow):
#   cmake -B build -DTORQUE_APP_NAME=Torque3D
#
# Umbrella path (default when TORQUE_APP_NAME is unset):
#   cmake -B build -DFUSE_UMBRELLA=ON
# -----------------------------------------------------------------------------
set(TORQUE_APP_NAME "" CACHE STRING "Torque3D application name (required for FUSE_BUILD_T3D)")

set(_fuse_umbrella_default ON)
if(NOT "${TORQUE_APP_NAME}" STREQUAL "")
    set(_fuse_umbrella_default OFF)
endif()
option(FUSE_UMBRELLA "Enable the FUSE umbrella build (fuse_core + optional legacy engines)" ${_fuse_umbrella_default})

if(NOT FUSE_UMBRELLA)
    include("${CMAKE_SOURCE_DIR}/cmake/LegacyTorque3D.cmake")
    return()
endif()

project(FUSE VERSION 0.1.0 LANGUAGES CXX)

# Umbrella/Engine default stays C++17. Product libs under Source/FUSE raise to 23.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_SOURCE_DIR}/cmake/FusePlatforms.cmake")
fuse_log_platform_profile()

option(FUSE_BUILD_CORE "Build fuse_core library and tests" ON)
option(FUSE_BUILD_CORE_TESTS "Build fuse_core unit tests (fuse_core_tests)" ON)
option(FUSE_BUILD_LEGACY "Build fuse_t3d_legacy / fuse_t2d_legacy quarantine libs" ON)
option(FUSE_BUILD_SMOKE "Build fuse_runtime_smoke one-process test binary" ON)
option(FUSE_BUILD_HYBRID_DEMO "Build demo_hybrid_hud U4 hybrid frame demo" ON)
option(FUSE_BUILD_PROJECT "Build fuse_project library + project.json loader (U7/WP-09)" ON)
option(FUSE_BUILD_PARITY_DEMOS "Build U8 parity demo binaries under Source/FUSE/Apps" ON)
option(FUSE_BUILD_TOOLS "Build Tools/FUSE CLI utilities (fuse_import dry-run)" ON)
option(FUSE_BUILD_AUDIO "Build fuse_audio spatial audio engine (B7.2)" ON)
option(FUSE_BUILD_SCRIPT "Build fuse_script Lua-ready host stubs (B7.3)" ON)
option(FUSE_BUILD_MODULES "Build fuse_* feature modules (U5)" ON)
option(FUSE_BUILD_EDITOR_API "Build fuse_editor_api command queue + editor host (U6/WP-08)" ON)
option(FUSE_BUILD_EDITOR "Build fuse_editor Qt 6 desktop shell (requires Qt6; desktop-only)" OFF)
option(FUSE_BUILD_VULKAN "Build fuse_rhi Vulkan bootstrap (Track B B2.1)" ON)
option(FUSE_PLATFORM_WINDOW_GLFW "Enable optional GLFW hidden-window WSI scaffold for desktop present path" OFF)
option(FUSE_ENABLE_GLFW_PRESENT "Allow vkQueuePresentKHR when display+GLFW WSI available (OFF for headless CI)" OFF)
option(FUSE_ENABLE_WIN32_PRESENT "Allow vkQueuePresentKHR when Win32 WSI available (OFF for headless CI)" OFF)
option(FUSE_ENABLE_QT_PRESENT "Allow vkQueuePresentKHR when display+Qt Vulkan surface available (OFF for headless CI)" OFF)
option(FUSE_SHIPPING "Shipping game runtime: no Qt, no editor shell, no legacy engine GUI" OFF)
option(FUSE_PROFILE "Enable profiler-friendly RelWithDebInfo defines" OFF)
option(FUSE_BUILD_CUDA "Build optional CUDA job lane, interop stubs, and fuse_compute (Track B B2.6–B2.7, B5.7 screen-space stubs)" OFF)
option(FUSE_SMOKE_ENABLE_ASAN "Enable AddressSanitizer for fuse_runtime_smoke" OFF)
option(FUSE_CORE_ENABLE_ASAN "Enable AddressSanitizer for fuse_core and fuse_core_* tests" OFF)
option(FUSE_CORE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for fuse_core and fuse_core_* tests" OFF)
option(FUSE_CORE_ENABLE_TSAN "Enable ThreadSanitizer for fuse_core and fuse_core_tests" OFF)
include("${CMAKE_SOURCE_DIR}/cmake/FuseSanitizers.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/FuseWarnings.cmake")
option(FUSE_BUILD_T3D "Build Torque3D application target(s)" ON)
option(FUSE_BUILD_T2D "Build Torque2D engine target (requires submodule)" ON)
option(FUSE_ENABLE_CLANG_TIDY "Export compile_commands.json for the optional fuse_clang_tidy seed gate (does not set CMAKE_CXX_CLANG_TIDY)" OFF)

if(FUSE_ENABLE_CLANG_TIDY)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

set(FUSE_CUDA_BACKEND FALSE)
if(FUSE_BUILD_CUDA)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        # Row B1 "CUDA compiles against C++23 FUSE host headers": host TUs stay C++23; nvcc 12.x
        # tops out at a C++20 device dialect, so .cu TUs use CUDA_STANDARD 20 and include the same
        # fuse/types.hpp, fuse/math/*, fuse/gria.hpp (FUSE_HOST_DEVICE) — no parallel CUDA types.
        # nvcc must use a host g++ it supports (CUDA 12.0: GCC <= 12); pass
        # -DCMAKE_CUDA_HOST_COMPILER=g++-12 when the default g++ is newer.
        if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
            set(CMAKE_CUDA_ARCHITECTURES 86)  # RTX 3090 (sm_86) per plan; compile-only CI needs no GPU
        endif()
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 20)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        set(CMAKE_CUDA_EXTENSIONS OFF)
        # std::array / std::max / std::min are constexpr and callable from FUSE_HOST_DEVICE code.
        string(APPEND CMAKE_CUDA_FLAGS " --expt-relaxed-constexpr")
        set(FUSE_CUDA_BACKEND TRUE)
        message(STATUS "FUSE: CUDA toolkit found — FUSE_HAS_CUDA enabled")
    else()
        message(STATUS "FUSE: FUSE_BUILD_CUDA=ON but toolkit not found — CUDA stub path only")
    endif()
endif()

# Policy first (force flags). Runtime target Qt-ban runs at the end of
# Source/FUSE and Apps/RuntimeSmoke after those targets exist.
include("${CMAKE_SOURCE_DIR}/cmake/FuseShipping.cmake")

if(FUSE_BUILD_T3D AND "${TORQUE_APP_NAME}" STREQUAL "")
    set(TORQUE_APP_NAME "Torque3D" CACHE STRING "Torque3D application name" FORCE)
    message(STATUS "FUSE: defaulting TORQUE_APP_NAME to Torque3D for FUSE_BUILD_T3D")
endif()

if(FUSE_BUILD_CORE)
    if(FUSE_BUILD_CORE_TESTS)
        enable_testing()
    endif()
    add_subdirectory(Source/FUSE)
endif()

if(FUSE_BUILD_T3D)
    if("${TORQUE_APP_NAME}" STREQUAL "")
        message(FATAL_ERROR "FUSE_BUILD_T3D requires TORQUE_APP_NAME")
    endif()
    include("${CMAKE_SOURCE_DIR}/cmake/LegacyTorque3D.cmake")
endif()

if(FUSE_BUILD_T2D)
    include("${CMAKE_SOURCE_DIR}/cmake/FuseTorque2D.cmake")
    fuse_configure_torque2d()
    if(NOT FUSE_T2D_AVAILABLE)
        message(STATUS "FUSE_BUILD_T2D=ON but Torque2D unavailable — continuing without T2D target")
    endif()
endif()

if(FUSE_BUILD_TOOLS AND FUSE_BUILD_PROJECT)
    add_subdirectory(Tools/FUSE)
endif()

# Installer / package: install() rules + CPack ("fuse", vendor "FUSE"; TGZ+DEB / NSIS+ZIP), app icon
# and desktop entry, with the fuse_package_gate ctest (Appendix A: icons, installer).
include("${CMAKE_SOURCE_DIR}/cmake/FusePackaging.cmake")

# Optional P7 clang-tidy seed gate (not CMAKE_CXX_CLANG_TIDY — too slow / breaks MSVC).
# Needs compile_commands.json for clang-tidy -p. Ninja/Makefile emit it when
# CMAKE_EXPORT_COMPILE_COMMANDS is ON (FUSE_ENABLE_CLANG_TIDY or fuse-debug / fuse-asan).
# cmake --build is recommended before `fuse_clang_tidy` / ctest -R fuse_p7_clang_tidy.
include("${CMAKE_SOURCE_DIR}/cmake/FuseClangTidy.cmake")

# Shipping: binary inspection of every fuse_* library / shipped executable (B7.10).
fuse_shipping_add_binary_strip_test()

# -Wall -Wextra (+ -Werror with FUSE_WARNINGS_AS_ERRORS) on every Source/FUSE +
# Tools/FUSE target; vendored Engine/lib subprojects excluded (cmake/FuseWarnings.cmake).
fuse_warnings_finalize()

# FUSE_SANITIZE (fuse-asan preset): instrument every Source/FUSE + Tools/FUSE
# target and wire sanitizer runtime options into their tests. Must run last.
fuse_sanitize_finalize()
