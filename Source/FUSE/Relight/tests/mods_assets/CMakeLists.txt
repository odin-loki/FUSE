# FUSE Relight RL-3.3 tests: mod asset readers. Added from Source/FUSE/Relight/mods/assets/CMakeLists.txt.
#
#   rl_mods_assets_unit       (relight;gate) decode vs expected for every format (known texels, known blocks, the
#                             bcdec oracle digests over every BC6H/BC7 mode), DDS DX10 round trips, every DX9
#                             FourCC / mask format, RL-1.8's DX9 writer read back byte-identical, GDeflate reference
#                             round trip, `.pkg` layout facts and writer -> reader round trips.
#   rl_mods_assets_fuzz       (relight;gate) 1000 malformed files per reader (DDS, package, GDeflate stream); no
#                             crash, both accept and reject paths exercised.
#   rl_mods_assets_fuzz_asan  (relight;gate;asan) the same fuzz, with the readers and the vendored codec compiled
#                             with -fsanitize=address,undefined. Native (non-cross) GNU/Clang trees only, when the
#                             toolchain links libasan and FUSE_SANITIZE is not already instrumenting everything.
# In a MinGW tree the executables are PE and run through CMAKE_CROSSCOMPILING_EMULATOR (Wine).

set(_rl_ma_targets fuse_relight_mods_assets_tests fuse_relight_mods_assets_fuzz)
add_executable(fuse_relight_mods_assets_tests test_mods_assets.cpp)
target_link_libraries(fuse_relight_mods_assets_tests PRIVATE fuse_relight_mods_assets)
if(TARGET fuse_relight_capture_export)
    # Cross-check against RL-1.8's DX9 DDS writer and canonical RGBA8 decode.
    target_link_libraries(fuse_relight_mods_assets_tests PRIVATE fuse_relight_capture_export)
    target_compile_definitions(fuse_relight_mods_assets_tests PRIVATE FUSE_RL_MODS_HAVE_CAPTURE_EXPORT=1)
else()
    target_compile_definitions(fuse_relight_mods_assets_tests PRIVATE FUSE_RL_MODS_HAVE_CAPTURE_EXPORT=0)
endif()
add_executable(fuse_relight_mods_assets_fuzz fuzz_mods_assets.cpp)
target_link_libraries(fuse_relight_mods_assets_fuzz PRIVATE fuse_relight_mods_assets)

add_test(NAME rl_mods_assets_unit COMMAND fuse_relight_mods_assets_tests)
set_tests_properties(rl_mods_assets_unit PROPERTIES LABELS "gate;relight" TIMEOUT 300)
add_test(NAME rl_mods_assets_fuzz COMMAND fuse_relight_mods_assets_fuzz 1000)
set_tests_properties(rl_mods_assets_fuzz PROPERTIES LABELS "gate;relight" TIMEOUT 600)

# AddressSanitizer + UBSan build of the fuzz target (sources compiled again, instrumented).
set(_rl_ma_asan OFF)
if(NOT CMAKE_CROSSCOMPILING AND "${FUSE_SANITIZE}" STREQUAL "" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXSourceCompiles)
    set(_rl_ma_saved_flags "${CMAKE_REQUIRED_FLAGS}")
    set(_rl_ma_saved_link "${CMAKE_REQUIRED_LINK_OPTIONS}")
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" FUSE_RL_MODS_ASAN_LINKS)
    set(CMAKE_REQUIRED_FLAGS "${_rl_ma_saved_flags}")
    set(CMAKE_REQUIRED_LINK_OPTIONS "${_rl_ma_saved_link}")
    set(_rl_ma_asan ${FUSE_RL_MODS_ASAN_LINKS})
endif()
if(_rl_ma_asan)
    set(_rl_ma_asan_sources fuzz_mods_assets.cpp ${FUSE_RELIGHT_MODS_ASSETS_SOURCES})
    if(FUSE_RELIGHT_GDEFLATE)
        list(APPEND _rl_ma_asan_sources ${FUSE_RELIGHT_GDEFLATE_SOURCES})
    endif()
    add_executable(fuse_relight_mods_assets_fuzz_asan ${_rl_ma_asan_sources})
    target_include_directories(fuse_relight_mods_assets_fuzz_asan PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../mods/assets/include")
    target_include_directories(fuse_relight_mods_assets_fuzz_asan SYSTEM PRIVATE ${FUSE_RELIGHT_GDEFLATE_INCLUDES})
    target_link_libraries(fuse_relight_mods_assets_fuzz_asan PRIVATE fuse_relight_hash)
    if(FUSE_RELIGHT_GDEFLATE)
        find_package(Threads REQUIRED)
        target_link_libraries(fuse_relight_mods_assets_fuzz_asan PRIVATE Threads::Threads)
        target_compile_definitions(fuse_relight_mods_assets_fuzz_asan PRIVATE FUSE_RELIGHT_HAS_GDEFLATE=1)
    else()
        target_compile_definitions(fuse_relight_mods_assets_fuzz_asan PRIVATE FUSE_RELIGHT_HAS_GDEFLATE=0)
    endif()
    target_compile_options(fuse_relight_mods_assets_fuzz_asan PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
                                                                      -fno-sanitize-recover=undefined)
    target_link_options(fuse_relight_mods_assets_fuzz_asan PRIVATE -fsanitize=address,undefined)
    list(APPEND _rl_ma_targets fuse_relight_mods_assets_fuzz_asan)
    add_test(NAME rl_mods_assets_fuzz_asan COMMAND fuse_relight_mods_assets_fuzz_asan 1000)
    set_tests_properties(rl_mods_assets_fuzz_asan PROPERTIES LABELS "gate;relight;asan" TIMEOUT 900
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endif()

foreach(_t IN LISTS _rl_ma_targets)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()
