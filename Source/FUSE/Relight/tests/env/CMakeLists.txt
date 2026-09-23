# FUSE Relight RL-0.3: environment probes for the Wine + Xvfb + Lavapipe test runner.
#
#   rl_env_probe64  x64 PE: vulkan-1.dll under Wine reaches Lavapipe ("llvmpipe") and the device
#                   exposes the ray-tracing extensions (plan §6.1). Fails if it does not.
#   rl_env_probe32  the same probe built for i686: passes in CI (wine32 + i686 MinGW installed);
#                   skips (77) where wine32 or the i686 compiler is missing.
#
# Both run through cmake/toolchains/fuse-wine-xvfb-run.sh (not the null-driver emulator
# fuse-wine-run.sh, which cannot load winevulkan). Labels: relight;wine.
# Wired in by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(tests/env)
if(NOT WIN32 OR NOT CMAKE_CROSSCOMPILING)
    # The probes check the Wine environment of a Linux-hosted cross build; a native Windows build
    # has no Wine to probe.
    return()
endif()

get_filename_component(_rl_env_repo "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(FUSE_WINE_XVFB_RUN "${_rl_env_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh"
    CACHE FILEPATH "Wine + Xvfb + Lavapipe runner for FUSE Relight tests")
set(FUSE_WINE_XVFB_PREFIX_ROOT "${CMAKE_BINARY_DIR}/wineprefix-xvfb"
    CACHE PATH "Prefix root (template + per-slot WINEPREFIXes) used by fuse-wine-xvfb-run.sh")

set(_rl_env_src "${CMAKE_CURRENT_SOURCE_DIR}/probe_vulkan.c")
set(_rl_env_flags -std=c99 -O2 -Wall -Wextra -Werror -static)

# Builds probe_vulkan.c for the architecture that is not the current toolchain's, with that
# architecture's MinGW driver; sets <out_var> to the .exe path, or "" when no compiler is found.
function(_rl_env_probe_other_arch triple suffix extra_flags out_var)
    find_program(_rl_env_cc_${suffix} NAMES ${triple}-gcc-posix ${triple}-gcc)
    if(NOT _rl_env_cc_${suffix})
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    set(_exe "${CMAKE_CURRENT_BINARY_DIR}/rl_env_probe_vulkan${suffix}.exe")
    add_custom_command(
        OUTPUT "${_exe}"
        COMMAND "${_rl_env_cc_${suffix}}" ${_rl_env_flags} ${extra_flags} -o "${_exe}" "${_rl_env_src}"
        DEPENDS "${_rl_env_src}"
        COMMENT "Building ${triple} env probe rl_env_probe_vulkan${suffix}.exe"
        VERBATIM)
    add_custom_target(rl_env_probe_vulkan${suffix} ALL DEPENDS "${_exe}")
    set(${out_var} "${_exe}" PARENT_SCOPE)
endfunction()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(rl_env_probe_vulkan64 "${_rl_env_src}")
    set_target_properties(rl_env_probe_vulkan64 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    set(_rl_env_exe64 "$<TARGET_FILE:rl_env_probe_vulkan64>")
    _rl_env_probe_other_arch(i686-w64-mingw32 32 "-msse2;-mfpmath=sse" _rl_env_exe32)
else()
    add_executable(rl_env_probe_vulkan32 "${_rl_env_src}")
    set_target_properties(rl_env_probe_vulkan32 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    set(_rl_env_exe32 "$<TARGET_FILE:rl_env_probe_vulkan32>")
    _rl_env_probe_other_arch(x86_64-w64-mingw32 64 "" _rl_env_exe64)
endif()

foreach(_bits 64 32)
    set(_name rl_env_probe${_bits})
    if(_rl_env_exe${_bits})
        # Lavapipe is the CI device; the check is that a Windows PE reaches it with RT extensions.
        add_test(NAME ${_name}
                 COMMAND "${FUSE_WINE_XVFB_RUN}" "${FUSE_WINE_XVFB_PREFIX_ROOT}" "${_rl_env_exe${_bits}}"
                         --expect-device llvmpipe)
    else()
        add_test(NAME ${_name}
                 COMMAND sh -c "echo 'SKIP: no MinGW compiler for the ${_bits}-bit probe'; exit 77")
    endif()
    # TIMEOUT covers the one-off prefix boot (about 20 s) on the first Wine test of a build tree.
    set_tests_properties(${_name} PROPERTIES
        LABELS "relight;wine"
        SKIP_RETURN_CODE 77
        TIMEOUT 300)
endforeach()
