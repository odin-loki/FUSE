# -----------------------------------------------------------------------------
# WP-0.6 (docs/unification/RENDERER-EXECUTION.md): optional Tracy profiler backend.
# Included once from Source/FUSE/Core/CMakeLists.txt after fuse_core exists.
#
#   FUSE_TRACY=OFF (default)  nothing Tracy-related is compiled into fuse_core or its users; the
#                             FUSE_PROFILE_* macros expand exactly as before.
#   FUSE_TRACY=ON             (fuse-profile preset) fuse_core links fuse_profiler_tracy, which
#                             PUBLIC-defines FUSE_TRACY=1 + TRACY_ENABLE: the profiler macros also
#                             emit Tracy zones / plots / frame marks and fuse::renderer::GpuProfiler
#                             emits TracyVk GPU zones.
#
# Targets (always defined when the vendored tree is present, so the stub build compiles and tests
# the enabled variant even with FUSE_TRACY=OFF):
#   fuse_tracy_client    Engine/lib/tracy/public/TracyClient.cpp (vendored, warnings off)
#   fuse_profiler_tracy  Source/FUSE/Core/src/profiler/tracy_adapter.cpp + the client
# -----------------------------------------------------------------------------
option(FUSE_TRACY "Build the FUSE profiler with the Tracy backend (vendored Engine/lib/tracy, BSD-3)" OFF)
option(FUSE_TRACY_ON_DEMAND "Tracy records only while a server is connected (TRACY_ON_DEMAND)" ON)
option(FUSE_TRACY_ONLY_LOCALHOST "Tracy listens on localhost only (TRACY_ONLY_LOCALHOST)" ON)

set(FUSE_TRACY_DIR "${CMAKE_SOURCE_DIR}/Engine/lib/tracy")
if(NOT EXISTS "${FUSE_TRACY_DIR}/public/TracyClient.cpp" OR NOT EXISTS "${FUSE_TRACY_DIR}/VERSION")
    if(FUSE_TRACY)
        message(FATAL_ERROR "FUSE_TRACY=ON but the vendored Tracy client is missing under ${FUSE_TRACY_DIR}")
    endif()
    return()
endif()
file(STRINGS "${FUSE_TRACY_DIR}/VERSION" _fuse_tracy_version_line REGEX "^version=")
string(REGEX REPLACE "^version=" "" FUSE_TRACY_VERSION "${_fuse_tracy_version_line}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FUSE_TRACY_DIR}/VERSION")

if(NOT TARGET fuse_tracy_client)
    find_package(Threads REQUIRED)
    add_library(fuse_tracy_client STATIC "${FUSE_TRACY_DIR}/public/TracyClient.cpp")
    target_compile_features(fuse_tracy_client PUBLIC cxx_std_17)
    # SYSTEM: third-party headers stay out of FUSE warning levels.
    target_include_directories(fuse_tracy_client SYSTEM PUBLIC "${FUSE_TRACY_DIR}/public")
    target_compile_definitions(fuse_tracy_client PUBLIC TRACY_ENABLE)
    if(FUSE_TRACY_ON_DEMAND)
        target_compile_definitions(fuse_tracy_client PUBLIC TRACY_ON_DEMAND)
    endif()
    if(FUSE_TRACY_ONLY_LOCALHOST)
        target_compile_definitions(fuse_tracy_client PUBLIC TRACY_ONLY_LOCALHOST)
    endif()
    set_target_properties(fuse_tracy_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(fuse_tracy_client PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(fuse_tracy_client PUBLIC ws2_32 dbghelp secur32 advapi32 user32)
        # ETW system tracing/sampling needs administrator rights and is unimplemented under Wine
        # (EnumerateTraceGuidsEx aborts); FUSE's zones and GPU timings do not depend on it.
        target_compile_definitions(fuse_tracy_client PUBLIC TRACY_NO_SYSTEM_TRACING)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties("${FUSE_TRACY_DIR}/public/TracyClient.cpp" PROPERTIES COMPILE_OPTIONS "-w")
    elseif(MSVC)
        set_source_files_properties("${FUSE_TRACY_DIR}/public/TracyClient.cpp" PROPERTIES COMPILE_OPTIONS "/W0")
    endif()

    add_library(fuse_profiler_tracy STATIC
        "${CMAKE_SOURCE_DIR}/Source/FUSE/Core/src/profiler/tracy_adapter.cpp"
        "${CMAKE_SOURCE_DIR}/Source/FUSE/Core/include/fuse/profiler/tracy_adapter.hpp")
    add_library(fuse::profiler_tracy ALIAS fuse_profiler_tracy)
    # fuse_core headers without linking fuse_core (fuse_core links this library when FUSE_TRACY=ON).
    target_include_directories(fuse_profiler_tracy PRIVATE "${CMAKE_SOURCE_DIR}/Source/FUSE/Core/include")
    target_include_directories(fuse_profiler_tracy SYSTEM PRIVATE "${FUSE_TRACY_DIR}")
    target_compile_definitions(fuse_profiler_tracy PUBLIC FUSE_TRACY=1)
    target_link_libraries(fuse_profiler_tracy PUBLIC fuse_tracy_client)
endif()

if(FUSE_TRACY AND FUSE_SHIPPING)
    message(FATAL_ERROR "FUSE_TRACY=ON conflicts with FUSE_SHIPPING (FUSE_NO_PROFILER strips every profiler macro)")
elseif(FUSE_TRACY)
    target_link_libraries(fuse_core PUBLIC fuse_profiler_tracy)
    message(STATUS "FUSE: profiler Tracy backend ON (vendored Tracy ${FUSE_TRACY_VERSION}, on-demand=${FUSE_TRACY_ON_DEMAND})")
else()
    message(STATUS "FUSE: profiler Tracy backend OFF (-DFUSE_TRACY=ON or the fuse-profile preset enables it)")
endif()

# Tests: the default-mode macro checks plus the Tracy-on variant (built even with FUSE_TRACY=OFF).
# Cross builds (MinGW) compile both variants; the nm checks run on native builds only.
if(FUSE_BUILD_CORE_TESTS)
    set(_fuse_tracy_test_src "${CMAKE_SOURCE_DIR}/Source/FUSE/Core/tests/test_profiler_tracy.cpp")
    set(_fuse_tracy_nm_check "${CMAKE_SOURCE_DIR}/cmake/FuseTracySymbolCheck.cmake")
    if(NOT CMAKE_CROSSCOMPILING)
        find_program(FUSE_NM_TOOL NAMES nm llvm-nm)
    endif()

    # The current mode (OFF in a default build): macros expand without Tracy (compile-time check).
    add_executable(fuse_core_profiler_tracy_mode_tests "${_fuse_tracy_test_src}")
    target_link_libraries(fuse_core_profiler_tracy_mode_tests PRIVATE fuse_core)
    if(FUSE_TRACY)
        target_compile_definitions(fuse_core_profiler_tracy_mode_tests PRIVATE FUSE_TRACY_TEST_EXPECT_ON=1)
    endif()
    add_test(NAME fuse_core_profiler_tracy_mode COMMAND fuse_core_profiler_tracy_mode_tests)
    set_tests_properties(fuse_core_profiler_tracy_mode PROPERTIES LABELS "gate;core;profiler")

    if(NOT FUSE_TRACY)
        # Zero overhead: no Tracy symbol in fuse_core or in a binary using every profiler macro.
        if(FUSE_NM_TOOL)
            add_test(NAME fuse_core_profiler_tracy_zero_overhead
                COMMAND "${CMAKE_COMMAND}" -DNM=${FUSE_NM_TOOL} -DEXPECT=absent
                        "-DFILES=$<TARGET_FILE:fuse_core>|$<TARGET_FILE:fuse_core_profiler_tracy_mode_tests>"
                        -P "${_fuse_tracy_nm_check}")
            set_tests_properties(fuse_core_profiler_tracy_zero_overhead PROPERTIES LABELS "gate;core;profiler")
        endif()

        # Enabled variant: the same test source compiled with FUSE_TRACY=1 against the same fuse_core.
        add_executable(fuse_core_profiler_tracy_on_tests "${_fuse_tracy_test_src}")
        target_link_libraries(fuse_core_profiler_tracy_on_tests PRIVATE fuse_core fuse_profiler_tracy)
        target_compile_definitions(fuse_core_profiler_tracy_on_tests PRIVATE FUSE_TRACY_TEST_EXPECT_ON=1)
        add_test(NAME fuse_core_profiler_tracy_on COMMAND fuse_core_profiler_tracy_on_tests)
        set_tests_properties(fuse_core_profiler_tracy_on PROPERTIES LABELS "gate;core;profiler")
        if(FUSE_NM_TOOL)
            # Negative control for the symbol check: the enabled binary must carry Tracy symbols.
            add_test(NAME fuse_core_profiler_tracy_on_symbols
                COMMAND "${CMAKE_COMMAND}" -DNM=${FUSE_NM_TOOL} -DEXPECT=present
                        "-DFILES=$<TARGET_FILE:fuse_core_profiler_tracy_on_tests>"
                        -P "${_fuse_tracy_nm_check}")
            set_tests_properties(fuse_core_profiler_tracy_on_symbols PROPERTIES LABELS "gate;core;profiler")
        endif()
    endif()
endif()
