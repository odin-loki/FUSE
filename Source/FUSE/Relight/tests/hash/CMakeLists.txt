# FUSE Relight RL-0.5 tests: Remix-compatible hash library (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1,
# §6.3 rl_hash_kat_*). Added from Source/FUSE/Relight/CMakeLists.txt with add_subdirectory(tests/hash)
# after add_subdirectory(hash).
#
#   rl_hash_kat        official xxHash vectors, the upstream-derived KAT table, property tests,
#                      library vs upstream oracle (verbatim dxvk-remix code, real SSE4.1 intrinsics)
#                      and the frozen digest of 10k random cases per function.
#   rl_hash_py_parity  the same 10k random cases per function (and the KAT table) recomputed by the
#                      independent Python reference Tools/FUSE/Relight/remix_hash_ref.py. Uses the
#                      `xxhash` wheel when installed, its own pure-Python xxHash otherwise.
#   rl_hash_wine       the KAT driver built with MinGW-w64 and run under Wine: it must pass every
#                      rl_hash_kat check, and its per-function digests of the random case set must
#                      equal the native build's and the golden file. Skips (77) without Wine or
#                      MinGW. In a MinGW cross build the driver itself runs under the Wine emulator.
if(NOT TARGET fuse_relight_hash)
    message(FATAL_ERROR "tests/hash needs fuse_relight_hash: add_subdirectory(hash) first")
endif()

set(_rl_hash_test_dir "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(_rl_hash_repo "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(_rl_hash_ref "${_rl_hash_repo}/Tools/FUSE/Relight/remix_hash_ref.py")
set(_rl_hash_test_sources test_hash_kat.cpp case_io.cpp cases.cpp upstream_oracle.cpp)

add_executable(fuse_relight_hash_kat ${_rl_hash_test_sources})
target_link_libraries(fuse_relight_hash_kat PRIVATE fuse_relight_hash)
target_include_directories(fuse_relight_hash_kat SYSTEM PRIVATE "${FUSE_RELIGHT_XXHASH_DIR}")
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_hash_kat)
else()
    set_target_properties(fuse_relight_hash_kat PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

set(_rl_hash_kat_args
    --vectors "${_rl_hash_test_dir}/xxhash_vectors.txt"
    --kat "${_rl_hash_test_dir}/kat_vectors.txt"
    --self
    --digest-check "${_rl_hash_test_dir}/digest_golden.txt")
add_test(NAME rl_hash_kat COMMAND fuse_relight_hash_kat ${_rl_hash_kat_args})
set_tests_properties(rl_hash_kat PROPERTIES LABELS "gate;relight;hash" TIMEOUT 600)

# ---- rl_hash_py_parity -----------------------------------------------------------------------------
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(_rl_hash_runner "")
    if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
        set(_rl_hash_runner --runner ${CMAKE_CROSSCOMPILING_EMULATOR})
    endif()
    add_test(NAME rl_hash_py_parity
        COMMAND "${Python3_EXECUTABLE}" "${_rl_hash_test_dir}/rl_hash_py_parity.py"
            --ref "${_rl_hash_ref}"
            --kat "${_rl_hash_test_dir}/kat_vectors.txt"
            --vectors "${_rl_hash_test_dir}/xxhash_vectors.txt"
            --work "${CMAKE_CURRENT_BINARY_DIR}/py_parity"
            --count 10000
            --exe "$<TARGET_FILE:fuse_relight_hash_kat>"
            ${_rl_hash_runner})
else()
    add_test(NAME rl_hash_py_parity COMMAND fuse_relight_hash_kat --skip "python3 not found")
endif()
set_tests_properties(rl_hash_py_parity PROPERTIES LABELS "gate;relight;hash" TIMEOUT 1500 SKIP_RETURN_CODE 77)

# ---- rl_hash_wine ----------------------------------------------------------------------------------
if(CMAKE_CROSSCOMPILING AND WIN32)
    # The driver is a Windows binary already; ctest runs it through CMAKE_CROSSCOMPILING_EMULATOR.
    add_test(NAME rl_hash_wine COMMAND fuse_relight_hash_kat ${_rl_hash_kat_args})
    set_tests_properties(rl_hash_wine PROPERTIES LABELS "gate;relight;hash;wine" TIMEOUT 900 SKIP_RETURN_CODE 77)
elseif(NOT WIN32)
    find_program(FUSE_RELIGHT_MINGW_CXX NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++)
    set(_rl_hash_mingw_exe "")
    if(FUSE_RELIGHT_MINGW_CXX)
        set(_rl_hash_mingw_exe "${CMAKE_CURRENT_BINARY_DIR}/fuse_relight_hash_kat_mingw.exe")
        get_target_property(_rl_hash_lib_sources fuse_relight_hash SOURCES)
        get_target_property(_rl_hash_lib_dir fuse_relight_hash SOURCE_DIR)
        set(_rl_hash_all_sources "")
        foreach(_s IN LISTS _rl_hash_lib_sources)
            list(APPEND _rl_hash_all_sources "${_rl_hash_lib_dir}/${_s}")
        endforeach()
        foreach(_s IN LISTS _rl_hash_test_sources)
            list(APPEND _rl_hash_all_sources "${_rl_hash_test_dir}/${_s}")
        endforeach()
        add_custom_command(
            OUTPUT "${_rl_hash_mingw_exe}"
            COMMAND "${FUSE_RELIGHT_MINGW_CXX}" -std=c++23 -O2 -Wall -Wextra -Werror -static
                    -I "${_rl_hash_lib_dir}/include" -isystem "${FUSE_RELIGHT_XXHASH_DIR}"
                    -o "${_rl_hash_mingw_exe}" ${_rl_hash_all_sources}
            DEPENDS ${_rl_hash_all_sources}
            COMMENT "Building the MinGW-w64 fuse_relight_hash_kat.exe for rl_hash_wine"
            VERBATIM)
        add_custom_target(fuse_relight_hash_kat_mingw ALL DEPENDS "${_rl_hash_mingw_exe}")
    endif()
    add_test(NAME rl_hash_wine
        COMMAND bash "${_rl_hash_test_dir}/rl_hash_wine.sh"
            "$<TARGET_FILE:fuse_relight_hash_kat>" "${_rl_hash_mingw_exe}"
            "${_rl_hash_repo}/cmake/toolchains/fuse-wine-run.sh" "${CMAKE_BINARY_DIR}/wineprefix"
            "${_rl_hash_test_dir}")
    set_tests_properties(rl_hash_wine PROPERTIES LABELS "gate;relight;hash;wine" TIMEOUT 900 SKIP_RETURN_CODE 77)
endif()
