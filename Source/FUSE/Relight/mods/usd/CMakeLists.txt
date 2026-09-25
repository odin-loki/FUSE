# FUSE Relight RL-3.1: the USD reader with the Remix profile (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.3, §4.4,
# Wave R3). Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(mods/usd). It also adds the
# package's tests (tests/) and the fixture tool (tools/).
#
# Targets
#   fuse_relight_tinyusdz   the vendored TinyUSDZ subset (Engine/lib/tinyusdz, Apache-2.0; Remaster W2.2): USDA and
#                           USDC (crate) layer readers, the crate writer (fixture generation) and what they need.
#                           Compiled verbatim; fuse_warnings_finalize gives files outside Source/FUSE `-w`, and the
#                           headers are SYSTEM includes for the FUSE code that includes them (src/layer_loader.cpp).
#   fuse_relight_mods_usd   layer model (TinyUSDZ PrimSpecs -> FUSE LayerData, values via USDA text), the Remix-profile
#                           composition engine (sublayers, references, payloads, variant sets, overs, instanceable
#                           prims flattened, asset paths anchored to the defining layer; everything else warns with
#                           the prim path), the canonical flattened dump, and the Remix view (mesh_/mat_/light_
#                           classification, remix_category:* flags, ParticleSystemAPI). Pure CPU: native and MinGW.
#   fuse_relight_usd_tool   CLI: `dump` (canonical flattened JSON), `to-usdc` (TinyUSDZ crate writer, used to generate
#                           the USDC fixtures at test time), `check` (fixture expectations).
# Tests: tests/ (rl_mod_usd_unit, rl_mod_capture_roundtrip, rl_mod_fixture_<name>_{usda,usdc},
#        rl_mod_flatten_check_selftest, rl_mod_flatten_crosscheck).

set(FUSE_RELIGHT_TINYUSDZ_DIR "${FUSE_VENDOR_DIR}/tinyusdz")

# The vendored subset (Engine/lib/tinyusdz/VERSION lists every file). Only what USDA + USDC layer reading, the
# value pretty-printer (values reach FUSE as USDA text) and the crate writer need.
set(_rl_tusd "${FUSE_RELIGHT_TINYUSDZ_DIR}/src")
set(FUSE_RELIGHT_TINYUSDZ_SOURCES
    ${_rl_tusd}/tinyusdz.cc
    ${_rl_tusd}/asset-resolution.cc
    ${_rl_tusd}/ascii-parser.cc
    ${_rl_tusd}/ascii-parser-props.cc
    ${_rl_tusd}/ascii-parser-entry.cc
    ${_rl_tusd}/ascii-parser-basetype.cc
    ${_rl_tusd}/ascii-parser-basetype-typedarray.cc
    ${_rl_tusd}/ascii-parser-timesamples.cc
    ${_rl_tusd}/ascii-parser-timesamples-array.cc
    ${_rl_tusd}/usda-reader.cc
    ${_rl_tusd}/usdc-reader.cc
    ${_rl_tusd}/usdc-reader-property.cc
    ${_rl_tusd}/usdc-reader-prim.cc
    ${_rl_tusd}/usdc-reader-reconstruct.cc
    ${_rl_tusd}/crate-reader.cc
    ${_rl_tusd}/crate-reader-arrays.cc
    ${_rl_tusd}/crate-reader-values.cc
    ${_rl_tusd}/crate-reader-paths.cc
    ${_rl_tusd}/crate-reader-timesamples.cc
    ${_rl_tusd}/crate-format.cc
    ${_rl_tusd}/crate-writer.cc
    ${_rl_tusd}/crate-path-utils/path_sort.cc
    ${_rl_tusd}/crate-path-utils/tree_encode.cc
    ${_rl_tusd}/usdc-writer.cc
    ${_rl_tusd}/sconv-layer.cc
    ${_rl_tusd}/pprint-ar.cc
    ${_rl_tusd}/pprint-media.cc
    ${_rl_tusd}/pprint-physics.cc
    ${_rl_tusd}/prim-reconstruct-ar.cc
    ${_rl_tusd}/prim-reconstruct-media.cc
    ${_rl_tusd}/stage-converter.cc
    ${_rl_tusd}/sconv-geom.cc
    ${_rl_tusd}/sconv-physics.cc
    ${_rl_tusd}/sconv-ar.cc
    ${_rl_tusd}/sconv-media.cc
    ${_rl_tusd}/sconv-shader.cc
    ${_rl_tusd}/sconv-light.cc
    ${_rl_tusd}/sconv-skel.cc
    ${_rl_tusd}/stage.cc
    ${_rl_tusd}/parser-timing.cc
    ${_rl_tusd}/prim-reconstruct-physics.cc
    ${_rl_tusd}/prim-types.cc
    ${_rl_tusd}/core/prim-enums.cc
    ${_rl_tusd}/core/instance-key.cc
    ${_rl_tusd}/enum-handlers.cc
    ${_rl_tusd}/layer.cc
    ${_rl_tusd}/primvar.cc
    ${_rl_tusd}/str-util.cc
    ${_rl_tusd}/value-pprint.cc
    ${_rl_tusd}/value-types.cc
    ${_rl_tusd}/tiny-format.cc
    ${_rl_tusd}/tiny-string.cc
    ${_rl_tusd}/io-util.cc
    ${_rl_tusd}/path-util.cc
    ${_rl_tusd}/prim-reconstruct.cc
    ${_rl_tusd}/prim-reconstruct-shader.cc
    ${_rl_tusd}/usdGeom.cc
    ${_rl_tusd}/usdSkel.cc
    ${_rl_tusd}/usdShade.cc
    ${_rl_tusd}/usdLux.cc
    ${_rl_tusd}/xform.cc
    ${_rl_tusd}/linear-algebra.cc
    ${_rl_tusd}/value-eval-util.cc
    ${_rl_tusd}/pprinter.cc
    ${_rl_tusd}/pprint-enum.cc
    ${_rl_tusd}/pprint-meta.cc
    ${_rl_tusd}/pprint-geom.cc
    ${_rl_tusd}/pprint-shader.cc
    ${_rl_tusd}/pprint-light.cc
    ${_rl_tusd}/pprint-skel.cc
    ${_rl_tusd}/timesamples.cc
    ${_rl_tusd}/timesamples-inst-scalar.cc
    ${_rl_tusd}/timesamples-inst-scalar-role.cc
    ${_rl_tusd}/timesamples-inst-array.cc
    ${_rl_tusd}/timesamples-inst-array-role.cc
    ${_rl_tusd}/timesamples-inst-array-basic.cc
    ${_rl_tusd}/timesamples-pprint.cc
    ${_rl_tusd}/typed-array.cc
    ${_rl_tusd}/hash-util.cc
    ${_rl_tusd}/integerCoding.cpp
    ${_rl_tusd}/lz4-compression.cc
    ${_rl_tusd}/lz4/lz4.c)

add_library(fuse_relight_tinyusdz STATIC ${FUSE_RELIGHT_TINYUSDZ_SOURCES})
target_include_directories(fuse_relight_tinyusdz SYSTEM PUBLIC "${_rl_tusd}" "${FUSE_RELIGHT_TINYUSDZ_DIR}")
target_include_directories(fuse_relight_tinyusdz PRIVATE "${_rl_tusd}/external")
# Upstream's CMake defaults for a library build without the optional modules we do not vendor (Tydra, MaterialX,
# JSON, image / audio loaders, zstd, C API).
target_compile_definitions(fuse_relight_tinyusdz PRIVATE TINYUSDZ_PRODUCTION_BUILD)
if(MSVC)
    target_compile_options(fuse_relight_tinyusdz PRIVATE /bigobj)
elseif(MINGW)
    # Several translation units exceed the PE/COFF section limit.
    target_compile_options(fuse_relight_tinyusdz PRIVATE -Wa,-mbig-obj)
endif()
find_package(Threads REQUIRED)
target_link_libraries(fuse_relight_tinyusdz PUBLIC Threads::Threads)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_tinyusdz)
else()
    set_target_properties(fuse_relight_tinyusdz PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_library(fuse_relight_mods_usd STATIC
    src/usd_value.cpp
    src/layer_loader.cpp
    src/compose.cpp
    src/canonical.cpp
    src/remix_profile.cpp)
target_include_directories(fuse_relight_mods_usd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_mods_usd PUBLIC fuse_relight_hash fuse_relight_classify
                                            PRIVATE fuse_relight_tinyusdz)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_mods_usd)
else()
    set_target_properties(fuse_relight_mods_usd PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_executable(fuse_relight_usd_tool tools/usd_tool.cpp)
target_link_libraries(fuse_relight_usd_tool PRIVATE fuse_relight_mods_usd fuse_relight_tinyusdz)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_usd_tool)
else()
    set_target_properties(fuse_relight_usd_tool PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(tests)
