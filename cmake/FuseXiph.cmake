# -----------------------------------------------------------------------------
# FUSE vendored xiph codecs (MP-B7.2-OGG-RUNTIME / AP-W8.3 / UNI-U7-AUDIO-1).
#
# Builds libogg 1.3.6, libvorbis 1.3.7 (vorbis, vorbisenc, vorbisfile) and libFLAC 1.5.0 as static
# libraries from the release archives vendored in vendor/ (xiph-*.tar.gz, BSD-3-Clause). No system
# library is ever used. The archives are checked against the raw sha256 pinned in
# vendor/xiph/<lib>/VERSION (archive_sha256), then extracted once into
# ${CMAKE_BINARY_DIR}/_fuse_xiph at configure time. `fuse_lint vendored-pins` checks the same pins
# (ctest fuse_lint_vendored_pins_xiph_{ogg,vorbis,flac}).
#
# Targets (C only, compiled with warnings off, like every other vendored library):
#   fuse_xiph_ogg        fuse::xiph_ogg
#   fuse_xiph_vorbis     fuse::xiph_vorbis      (links ogg)
#   fuse_xiph_vorbisenc  fuse::xiph_vorbisenc   (links vorbis)
#   fuse_xiph_vorbisfile fuse::xiph_vorbisfile  (links vorbis)
#   fuse_xiph_flac       fuse::xiph_flac        (native FLAC only, FLAC__NO_DLL, no SIMD asm)
# Results (global):
#   FUSE_HAS_OGG_VORBIS  ON when ogg + vorbis + vorbisenc + vorbisfile are available
#   FUSE_HAS_FLAC        ON when libFLAC is available
# Consumers link the targets and add the FUSE_HAS_* compile definitions themselves (fuse_audio does).
# Only fuse:: aliases are created (no Ogg::ogg / Vorbis::* names) so a later find_package() of a system
# copy elsewhere in the tree can never collide with these targets.
# -----------------------------------------------------------------------------
include_guard(GLOBAL)

option(FUSE_XIPH_CODECS "Build the vendored xiph codecs (Ogg Vorbis, FLAC) for fuse_audio" ON)

set(FUSE_HAS_OGG_VORBIS OFF)
set(FUSE_HAS_FLAC OFF)

function(_fuse_xiph_read_pin lib out_archive out_sha out_version)
    set(_pin "${FUSE_VENDOR_DIR}/xiph/${lib}/VERSION")
    if(NOT EXISTS "${_pin}")
        set(${out_archive} "" PARENT_SCOPE)
        return()
    endif()
    file(STRINGS "${_pin}" _lines REGEX "^(archive|archive_sha256|version)=")
    set(_archive "")
    set(_sha "")
    set(_ver "")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "^archive=(.+)$")
            set(_archive "${CMAKE_MATCH_1}")
        elseif(_l MATCHES "^archive_sha256=([0-9a-f]+)$")
            set(_sha "${CMAKE_MATCH_1}")
        elseif(_l MATCHES "^version=(.+)$")
            set(_ver "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${out_archive} "${_archive}" PARENT_SCOPE)
    set(${out_sha} "${_sha}" PARENT_SCOPE)
    set(${out_version} "${_ver}" PARENT_SCOPE)
endfunction()

# Extract vendor/<archive> (sha256-checked) into _fuse_xiph; sets <out_dir> to the unpacked root or "".
function(_fuse_xiph_extract lib top_dir out_dir)
    set(${out_dir} "" PARENT_SCOPE)
    _fuse_xiph_read_pin(${lib} _archive _sha _ver)
    if(_archive STREQUAL "" OR _sha STREQUAL "")
        message(STATUS "FUSE: xiph ${lib}: no pin in vendor/xiph/${lib}/VERSION")
        return()
    endif()
    set(_tar "${FUSE_VENDOR_DIR}/${_archive}")
    if(NOT EXISTS "${_tar}")
        message(STATUS "FUSE: xiph ${lib}: archive ${_archive} missing")
        return()
    endif()
    set(_root "${CMAKE_BINARY_DIR}/_fuse_xiph")
    set(_stamp "${_root}/${top_dir}.sha256")
    set(_have "")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _have)
        string(STRIP "${_have}" _have)
    endif()
    if(NOT _have STREQUAL _sha OR NOT IS_DIRECTORY "${_root}/${top_dir}")
        file(SHA256 "${_tar}" _actual)
        if(NOT _actual STREQUAL _sha)
            message(FATAL_ERROR "FUSE: ${_archive} sha256 ${_actual} does not match the pin ${_sha} "
                                "(vendor/xiph/${lib}/VERSION)")
        endif()
        file(REMOVE_RECURSE "${_root}/${top_dir}")
        file(MAKE_DIRECTORY "${_root}")
        file(ARCHIVE_EXTRACT INPUT "${_tar}" DESTINATION "${_root}")
        if(NOT IS_DIRECTORY "${_root}/${top_dir}")
            message(FATAL_ERROR "FUSE: ${_archive} did not unpack to ${top_dir}/")
        endif()
        file(WRITE "${_stamp}" "${_sha}\n")
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_tar}" "${FUSE_VENDOR_DIR}/xiph/${lib}/VERSION")
    set(${out_dir} "${_root}/${top_dir}" PARENT_SCOPE)
endfunction()

function(_fuse_xiph_quiet target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W0)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_SECURE_NO_DEPRECATE
                                                     _CRT_NONSTDC_NO_DEPRECATE)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "vendor/xiph")
endfunction()

if(FUSE_XIPH_CODECS)
    enable_language(C)

    _fuse_xiph_extract(ogg ogg-1.3.6 _fuse_ogg_dir)
    _fuse_xiph_extract(vorbis vorbis-1.3.7 _fuse_vorbis_dir)
    _fuse_xiph_extract(flac flac-1.5.0 _fuse_flac_dir)

    # ---- libogg ------------------------------------------------------------------------------------
    if(_fuse_ogg_dir AND NOT TARGET fuse_xiph_ogg)
        set(_gen "${CMAKE_BINARY_DIR}/_fuse_xiph/generated/ogg")
        file(MAKE_DIRECTORY "${_gen}/ogg")
        # config_types.h.in filled for <stdint.h> platforms (every FUSE target: GCC, Clang, MSVC, MinGW).
        set(INCLUDE_INTTYPES_H 0)
        set(INCLUDE_STDINT_H 1)
        set(INCLUDE_SYS_TYPES_H 0)
        set(SIZE16 int16_t)
        set(USIZE16 uint16_t)
        set(SIZE32 int32_t)
        set(USIZE32 uint32_t)
        set(SIZE64 int64_t)
        set(USIZE64 uint64_t)
        configure_file("${_fuse_ogg_dir}/include/ogg/config_types.h.in" "${_gen}/ogg/config_types.h" @ONLY)
        add_library(fuse_xiph_ogg STATIC
            "${_fuse_ogg_dir}/src/bitwise.c"
            "${_fuse_ogg_dir}/src/framing.c")
        target_include_directories(fuse_xiph_ogg SYSTEM PUBLIC "${_fuse_ogg_dir}/include" "${_gen}")
        _fuse_xiph_quiet(fuse_xiph_ogg)
        add_library(fuse::xiph_ogg ALIAS fuse_xiph_ogg)
    endif()

    # ---- libvorbis / vorbisenc / vorbisfile ---------------------------------------------------------
    if(_fuse_vorbis_dir AND TARGET fuse_xiph_ogg AND NOT TARGET fuse_xiph_vorbis)
        set(_vl "${_fuse_vorbis_dir}/lib")
        add_library(fuse_xiph_vorbis STATIC
            "${_vl}/mdct.c" "${_vl}/smallft.c" "${_vl}/block.c" "${_vl}/envelope.c" "${_vl}/window.c"
            "${_vl}/lsp.c" "${_vl}/lpc.c" "${_vl}/analysis.c" "${_vl}/synthesis.c" "${_vl}/psy.c"
            "${_vl}/info.c" "${_vl}/floor1.c" "${_vl}/floor0.c" "${_vl}/res0.c" "${_vl}/mapping0.c"
            "${_vl}/registry.c" "${_vl}/codebook.c" "${_vl}/sharedbook.c" "${_vl}/lookup.c"
            "${_vl}/bitrate.c")
        target_include_directories(fuse_xiph_vorbis SYSTEM PUBLIC "${_fuse_vorbis_dir}/include")
        target_include_directories(fuse_xiph_vorbis PRIVATE "${_vl}")
        target_link_libraries(fuse_xiph_vorbis PUBLIC fuse_xiph_ogg)
        if(NOT WIN32)
            target_link_libraries(fuse_xiph_vorbis PUBLIC m)
        endif()
        _fuse_xiph_quiet(fuse_xiph_vorbis)

        add_library(fuse_xiph_vorbisenc STATIC "${_vl}/vorbisenc.c")
        target_include_directories(fuse_xiph_vorbisenc PRIVATE "${_vl}")
        target_link_libraries(fuse_xiph_vorbisenc PUBLIC fuse_xiph_vorbis)
        _fuse_xiph_quiet(fuse_xiph_vorbisenc)

        add_library(fuse_xiph_vorbisfile STATIC "${_vl}/vorbisfile.c")
        target_link_libraries(fuse_xiph_vorbisfile PUBLIC fuse_xiph_vorbis)
        _fuse_xiph_quiet(fuse_xiph_vorbisfile)

        add_library(fuse::xiph_vorbis ALIAS fuse_xiph_vorbis)
        add_library(fuse::xiph_vorbisenc ALIAS fuse_xiph_vorbisenc)
        add_library(fuse::xiph_vorbisfile ALIAS fuse_xiph_vorbisfile)
    endif()

    # ---- libFLAC (native FLAC; the Ogg-FLAC mapping is not needed) ----------------------------------
    if(_fuse_flac_dir AND NOT TARGET fuse_xiph_flac)
        set(_fl "${_fuse_flac_dir}/src/libFLAC")
        set(_gen "${CMAKE_BINARY_DIR}/_fuse_xiph/generated/flac")
        file(MAKE_DIRECTORY "${_gen}")
        if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
            set(_be 1)
        else()
            set(_be 0)
        endif()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_w64 1)
        else()
            set(_w64 0)
        endif()
        # Hand-written config.h (FLAC's own CMake probes are not run): portable C, no x86/NEON intrinsics
        # (FLAC__NO_ASM), no pthread encoder threads, no Ogg mapping.
        file(CONFIGURE OUTPUT "${_gen}/config.h" CONTENT
"/* Generated by cmake/FuseXiph.cmake for the vendored libFLAC 1.5.0. */
#define CPU_IS_BIG_ENDIAN ${_be}
#define CPU_IS_LITTLE_ENDIAN (!${_be})
#define ENABLE_64_BIT_WORDS ${_w64}
#define OGG_FOUND 0
#define FLAC__HAS_OGG 0
#define FLAC__HAS_X86INTRIN 0
#define FLAC__HAS_NEONINTRIN 0
#define FLAC__HAS_A64NEONINTRIN 0
#define HAVE_LROUND 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define PACKAGE_VERSION \"1.5.0\"
" @ONLY)
        set(_flac_sources
            bitmath.c bitreader.c bitwriter.c cpu.c crc.c fixed.c float.c format.c lpc.c md5.c memory.c
            metadata_iterators.c metadata_object.c stream_decoder.c stream_encoder.c
            stream_encoder_framing.c window.c)
        list(TRANSFORM _flac_sources PREPEND "${_fl}/")
        if(WIN32)
            list(APPEND _flac_sources "${_fuse_flac_dir}/src/share/win_utf8_io/win_utf8_io.c")
        endif()
        add_library(fuse_xiph_flac STATIC ${_flac_sources})
        target_include_directories(fuse_xiph_flac SYSTEM PUBLIC "${_fuse_flac_dir}/include")
        target_include_directories(fuse_xiph_flac PRIVATE "${_gen}" "${_fl}/include")
        target_compile_definitions(fuse_xiph_flac PUBLIC FLAC__NO_DLL PRIVATE HAVE_CONFIG_H FLAC__NO_ASM)
        if(NOT WIN32)
            target_link_libraries(fuse_xiph_flac PUBLIC m)
        endif()
        _fuse_xiph_quiet(fuse_xiph_flac)
        add_library(fuse::xiph_flac ALIAS fuse_xiph_flac)
    endif()

    if(TARGET fuse_xiph_vorbisfile AND TARGET fuse_xiph_vorbisenc)
        set(FUSE_HAS_OGG_VORBIS ON)
    endif()
    if(TARGET fuse_xiph_flac)
        set(FUSE_HAS_FLAC ON)
    endif()
    message(STATUS "FUSE: vendored xiph codecs: ogg/vorbis=${FUSE_HAS_OGG_VORBIS} flac=${FUSE_HAS_FLAC}")
endif()
