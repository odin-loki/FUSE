# UNI-U7-ASSET-1 CPU gates (labels asset;gate;tsan — the TSan nightly builds Source/FUSE/Asset/tests/all
# and runs ^fuse_asset_runtime_ next to ^fuse_core_). Suites: id, formats, registry, cancel, mpsc
# (always); material (renderer .fusemat library present); cook (Tools/FUSE/Cook present: cooks a glTF
# cube + PNG with fuse_cook and loads them through the AssetRegistry).
#
# The renderer (fuse_material_layers, fuse_meshoptimizer) and the cook (fuse_cook_stubs, fuse_cook) are
# configured after this directory, so the optional links resolve at generate time and the cook suite is
# registered from a deferred call at the end of the top-level configure.

add_executable(fuse_asset_runtime_tests test_asset_runtime.cpp)
target_link_libraries(fuse_asset_runtime_tests PRIVATE fuse_asset fuse_core fuse_ecs
    $<$<TARGET_EXISTS:fuse_cook_stubs>:fuse_cook_stubs>
    $<$<TARGET_EXISTS:fuse_material_layers>:fuse_material_layers>)
target_compile_definitions(fuse_asset_runtime_tests PRIVATE
    "FUSE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
    $<$<TARGET_EXISTS:fuse_cook_stubs>:FUSE_ASSET_TEST_HAS_COOK=1>
    $<$<TARGET_EXISTS:fuse_material_layers>:FUSE_ASSET_TEST_HAS_FUSEMAT=1>)
fuse_apply_cxx23(fuse_asset_runtime_tests)
if(COMMAND fuse_core_apply_tsan)
    fuse_core_apply_tsan(fuse_asset_runtime_tests)
endif()

set(_fuse_asset_runtime_tests)
foreach(_suite id formats registry cancel mpsc material)
    add_test(NAME fuse_asset_runtime_${_suite} COMMAND fuse_asset_runtime_tests ${_suite})
    list(APPEND _fuse_asset_runtime_tests fuse_asset_runtime_${_suite})
endforeach()
set_tests_properties(${_fuse_asset_runtime_tests} PROPERTIES LABELS "asset;gate;tsan" TIMEOUT 300 SKIP_RETURN_CODE 77)

function(_fuse_asset_runtime_register_cook_suite)
    if(NOT TARGET fuse_cook_stubs)
        return()
    endif()
    set(_args cook)
    if(TARGET fuse_cook)
        add_dependencies(fuse_asset_runtime_tests fuse_cook)
        list(APPEND _args "$<TARGET_FILE:fuse_cook>")
    endif()
    add_test(NAME fuse_asset_runtime_cook COMMAND fuse_asset_runtime_tests ${_args})
    set_tests_properties(fuse_asset_runtime_cook PROPERTIES LABELS "asset;gate" TIMEOUT 600 SKIP_RETURN_CODE 77)
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _fuse_asset_runtime_register_cook_suite)

# fuse_asset stays free of assimp / stb / BCn encoders / the renderer (checked on the configured graph).
add_test(NAME fuse_asset_runtime_links
    COMMAND "${CMAKE_COMMAND}" "-DFUSE_LINT_MANIFEST=${CMAKE_BINARY_DIR}/fuse_lint/targets.manifest"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_asset_links.cmake")
set_tests_properties(fuse_asset_runtime_links PROPERTIES LABELS "asset;gate;lint" TIMEOUT 60)
