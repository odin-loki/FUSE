# AddressSanitizer application for fuse-asan / FUSE_*_ENABLE_ASAN.
# Flags are GNU/Clang (and MSVC /fsanitize=address) only. The fuse-asan
# preset still configures when libasan is missing — common on Windows MinGW —
# so fuse-debug is unaffected. Options stay ON; flags are skipped with STATUS.

set(FUSE_ASAN_SUPPORTED OFF)

if(FUSE_CORE_ENABLE_ASAN OR FUSE_SMOKE_ENABLE_ASAN)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(FUSE_ASAN_SUPPORTED ON)
        message(STATUS "FUSE: AddressSanitizer enabled (MSVC /fsanitize=address)")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        include(CheckCXXSourceCompiles)
        set(_fuse_asan_saved_req_flags "${CMAKE_REQUIRED_FLAGS}")
        set(_fuse_asan_saved_req_link "${CMAKE_REQUIRED_LINK_OPTIONS}")
        set(_fuse_asan_saved_quiet "${CMAKE_REQUIRED_QUIET}")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
        set(CMAKE_REQUIRED_LINK_OPTIONS ${CMAKE_REQUIRED_LINK_OPTIONS} -fsanitize=address)
        set(CMAKE_REQUIRED_QUIET TRUE)
        unset(FUSE_ASAN_LINK_OK CACHE)
        check_cxx_source_compiles("int main() { return 0; }" FUSE_ASAN_LINK_OK)
        set(CMAKE_REQUIRED_FLAGS "${_fuse_asan_saved_req_flags}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "${_fuse_asan_saved_req_link}")
        set(CMAKE_REQUIRED_QUIET "${_fuse_asan_saved_quiet}")
        if(FUSE_ASAN_LINK_OK)
            set(FUSE_ASAN_SUPPORTED ON)
            message(STATUS "FUSE: AddressSanitizer enabled (-fsanitize=address)")
        else()
            message(STATUS
                "FUSE: ASan requested but this toolchain cannot link libasan "
                "(common on Windows MinGW) — skipping sanitizer flags")
        endif()
    else()
        message(STATUS
            "FUSE: ASan requested but compiler is ${CMAKE_CXX_COMPILER_ID}; "
            "sanitizer flags are GNU/Clang-only — skipping")
    endif()
endif()

function(fuse_apply_asan target)
    # FUSE_SANITIZE (below) already instruments every FUSE target.
    if(NOT "${FUSE_SANITIZE}" STREQUAL "")
        return()
    endif()
    if(NOT FUSE_SMOKE_ENABLE_ASAN OR NOT FUSE_ASAN_SUPPORTED)
        return()
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "fuse_apply_asan: '${target}' is not a CMake target")
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /fsanitize=address)
        return()
    endif()
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
endfunction()

# -----------------------------------------------------------------------------
# FUSE_SANITIZE — repo-wide sanitizer switch for every FUSE-owned target.
#
#   cmake --preset fuse-asan            # FUSE_SANITIZE=address,undefined
#   cmake -B build/x -DFUSE_SANITIZE=address,undefined
#
# Applied once, at the end of the top-level CMakeLists (fuse_sanitize_finalize),
# to every buildsystem target whose SOURCE_DIR lives under Source/FUSE or
# Tools/FUSE (libraries, tests, demos, tools). Vendored third-party subprojects
# (Engine/lib/*: openal-soft, assimp, bullet, ENet, Lua) are NOT instrumented —
# they are linked into sanitized executables but compiled normally, which keeps
# UBSan findings scoped to FUSE code. Every ctest registered from those
# directories gets ASAN_/UBSAN_/LSAN_OPTIONS pointing at
# cmake/sanitizers/*.supp (third-party-only suppressions).
#
# UBSan is built with -fno-sanitize-recover=undefined so any finding aborts the
# test. Sources may test FUSE_SANITIZE_ADDRESS / FUSE_SANITIZE_UNDEFINED (both
# defined 0/1 on sanitized targets) — see fuse/core/sanitizer.hpp.
# -----------------------------------------------------------------------------
set(FUSE_SANITIZE "" CACHE STRING
    "Comma list of sanitizers applied to all FUSE targets + tests (address,undefined). Empty = off.")

set(FUSE_SANITIZE_ACTIVE OFF)
set(FUSE_SANITIZE_LIST)
if(NOT "${FUSE_SANITIZE}" STREQUAL "")
    string(REPLACE "," ";" FUSE_SANITIZE_LIST "${FUSE_SANITIZE}")
    foreach(_s IN LISTS FUSE_SANITIZE_LIST)
        if(NOT _s MATCHES "^(address|undefined)$")
            message(FATAL_ERROR "FUSE_SANITIZE: unsupported sanitizer '${_s}' (use address and/or undefined; TSan is FUSE_CORE_ENABLE_TSAN)")
        endif()
    endforeach()
    if(FUSE_CORE_ENABLE_TSAN)
        message(FATAL_ERROR "FUSE_SANITIZE and FUSE_CORE_ENABLE_TSAN are mutually exclusive")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(STATUS "FUSE: FUSE_SANITIZE=${FUSE_SANITIZE} ignored on ${CMAKE_CXX_COMPILER_ID} (GNU/Clang only)")
    else()
        include(CheckCXXSourceCompiles)
        set(_fuse_san_saved_req_flags "${CMAKE_REQUIRED_FLAGS}")
        set(_fuse_san_saved_req_link "${CMAKE_REQUIRED_LINK_OPTIONS}")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=${FUSE_SANITIZE}")
        set(CMAKE_REQUIRED_LINK_OPTIONS ${CMAKE_REQUIRED_LINK_OPTIONS} -fsanitize=${FUSE_SANITIZE})
        set(CMAKE_REQUIRED_QUIET TRUE)
        unset(FUSE_SANITIZE_LINK_OK CACHE)
        check_cxx_source_compiles("int main() { return 0; }" FUSE_SANITIZE_LINK_OK)
        set(CMAKE_REQUIRED_FLAGS "${_fuse_san_saved_req_flags}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "${_fuse_san_saved_req_link}")
        unset(CMAKE_REQUIRED_QUIET)
        if(FUSE_SANITIZE_LINK_OK)
            set(FUSE_SANITIZE_ACTIVE ON)
            message(STATUS "FUSE: FUSE_SANITIZE=${FUSE_SANITIZE} applied to all Source/FUSE + Tools/FUSE targets")
        else()
            message(STATUS "FUSE: FUSE_SANITIZE=${FUSE_SANITIZE} requested but toolchain cannot link it — skipping")
        endif()
    endif()
endif()

set(FUSE_SANITIZE_VK_ICD "/usr/share/vulkan/icd.d/lvp_icd.json" CACHE FILEPATH
    "Vulkan ICD manifest forced for tests in FUSE_SANITIZE builds (Lavapipe). Empty = loader default.")

set(FUSE_SANITIZER_SUPP_DIR "${CMAKE_CURRENT_LIST_DIR}/sanitizers")

function(_fuse_sanitize_collect dir out_targets out_dirs)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    set(_all_t ${_targets})
    set(_all_d "${dir}")
    foreach(_sd IN LISTS _subdirs)
        _fuse_sanitize_collect("${_sd}" _t _d)
        list(APPEND _all_t ${_t})
        list(APPEND _all_d ${_d})
    endforeach()
    set(${out_targets} ${_all_t} PARENT_SCOPE)
    set(${out_dirs} ${_all_d} PARENT_SCOPE)
endfunction()

function(_fuse_sanitize_is_fuse_dir path out)
    set(_ok OFF)
    foreach(_root "${CMAKE_SOURCE_DIR}/Source/FUSE" "${CMAKE_SOURCE_DIR}/Tools/FUSE")
        string(FIND "${path}/" "${_root}/" _pos)
        if(_pos EQUAL 0)
            set(_ok ON)
        endif()
    endforeach()
    set(${out} ${_ok} PARENT_SCOPE)
endfunction()

# Call once at the end of the top-level CMakeLists.txt.
function(fuse_sanitize_finalize)
    if(NOT FUSE_SANITIZE_ACTIVE)
        return()
    endif()
    list(JOIN FUSE_SANITIZE_LIST "," _csv)
    set(_has_asan 0)
    set(_has_ubsan 0)
    if("address" IN_LIST FUSE_SANITIZE_LIST)
        set(_has_asan 1)
    endif()
    if("undefined" IN_LIST FUSE_SANITIZE_LIST)
        set(_has_ubsan 1)
    endif()
    set(_copts -fsanitize=${_csv} -fno-omit-frame-pointer)
    if(_has_ubsan)
        list(APPEND _copts -fno-sanitize-recover=undefined)
    endif()

    _fuse_sanitize_collect("${CMAKE_SOURCE_DIR}" _targets _dirs)
    set(_count 0)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(NOT _type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
            continue()
        endif()
        get_target_property(_imported ${_t} IMPORTED)
        if(_imported)
            continue()
        endif()
        get_target_property(_sdir ${_t} SOURCE_DIR)
        _fuse_sanitize_is_fuse_dir("${_sdir}" _is_fuse)
        # Executables/shared objects anywhere that link a sanitized FUSE lib
        # still need the runtime; only instrument compile for FUSE dirs.
        if(_is_fuse)
            target_compile_options(${_t} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:${_copts}>)
            target_compile_definitions(${_t} PRIVATE
                FUSE_SANITIZE_ADDRESS=${_has_asan}
                FUSE_SANITIZE_UNDEFINED=${_has_ubsan})
            math(EXPR _count "${_count} + 1")
        endif()
        if(_is_fuse AND _type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$")
            target_link_options(${_t} PRIVATE -fsanitize=${_csv})
        endif()
    endforeach()

    # Test environment: runtime options + third-party suppressions.
    set(_env
        "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:halt_on_error=1:strict_init_order=1:detect_stack_use_after_return=0:check_initialization_order=1:suppressions=${FUSE_SANITIZER_SUPP_DIR}/asan.supp"
        "LSAN_OPTIONS=suppressions=${FUSE_SANITIZER_SUPP_DIR}/lsan.supp:print_suppressions=0"
        "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:suppressions=${FUSE_SANITIZER_SUPP_DIR}/ubsan.supp"
        "FUSE_SANITIZER_BUILD=${_csv}")
    # Pin the Vulkan loader to Lavapipe. Without it the loader probes every
    # installed Mesa ICD (intel/radeon/nouveau/...); those uninstrumented drivers
    # leak thread-start allocations and are dlclose()d before LSan reports, so
    # the leak stacks cannot even be symbolized or suppressed.
    if(FUSE_SANITIZE_VK_ICD AND EXISTS "${FUSE_SANITIZE_VK_ICD}")
        list(APPEND _env "VK_ICD_FILENAMES=${FUSE_SANITIZE_VK_ICD}" "VK_DRIVER_FILES=${FUSE_SANITIZE_VK_ICD}")
    endif()
    set(_tests_total 0)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(WARNING "FUSE_SANITIZE: CMake < 3.28 cannot set test ENVIRONMENT across directories; "
            "export ASAN_OPTIONS/UBSAN_OPTIONS/LSAN_OPTIONS manually (see cmake/FuseSanitizers.cmake)")
        set(_dirs)
    endif()
    foreach(_d IN LISTS _dirs)
        _fuse_sanitize_is_fuse_dir("${_d}" _is_fuse)
        if(NOT _is_fuse)
            continue()
        endif()
        get_property(_tests DIRECTORY "${_d}" PROPERTY TESTS)
        foreach(_test IN LISTS _tests)
            set_property(TEST "${_test}" DIRECTORY "${_d}" APPEND PROPERTY ENVIRONMENT ${_env})
            math(EXPR _tests_total "${_tests_total} + 1")
        endforeach()
    endforeach()
    message(STATUS "FUSE: FUSE_SANITIZE instrumented ${_count} targets, ${_tests_total} tests")
endfunction()
