# fuse_assetcheck: FUSE_ASSET_PLAN §5.3 validation gates (Wave 0 task W0.6), CPU only.
# Included from cmake/FuseLintGates.cmake (after the FUSE_BUILD_CORE_TESTS guard).
#
# One ctest per gate, labels asset;gate: `fuse_assetcheck fixture-test --gate <g>` writes the gate's
# passing fixture (which must pass every gate) and its failing fixtures (each must be rejected by that
# gate with the expected finding) to the build tree and checks them from disk.
# Real assets: `fuse_assetcheck check <asset.json> [--gates ...]` (descriptor format in the tool header).

add_executable(fuse_assetcheck "${CMAKE_SOURCE_DIR}/Tools/FUSE/AssetCheck/fuse_assetcheck.cpp")
set_target_properties(fuse_assetcheck PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# stb_image (public domain / MIT) decodes PNG / TGA / JPEG sources; without it only raw TGA is read.
set(_fuse_assetcheck_stb "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/loaders/stb")
if(EXISTS "${_fuse_assetcheck_stb}/stb_image.h")
    target_include_directories(fuse_assetcheck SYSTEM PRIVATE "${_fuse_assetcheck_stb}")
    target_compile_definitions(fuse_assetcheck PRIVATE FUSE_ASSETCHECK_HAS_STB=1)
endif()
target_compile_options(fuse_assetcheck PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O2>)

set(_fuse_assetcheck_gates naming budget_mesh lod_chain budget_texture texel_density normal_map
    color_space pbr_sanity uv_bounds geometry)
foreach(_gate IN LISTS _fuse_assetcheck_gates)
    add_test(NAME fuse_assetcheck_${_gate}
             COMMAND fuse_assetcheck fixture-test --gate ${_gate}
                     --scratch "${CMAKE_BINARY_DIR}/fuse_assetcheck/fixtures")
    set_tests_properties(fuse_assetcheck_${_gate} PROPERTIES LABELS "asset;gate" TIMEOUT 120)
endforeach()
