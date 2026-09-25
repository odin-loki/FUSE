add_executable(fuse_bc7_encoder_tests
    test_bc7_encoder.cpp
)

target_link_libraries(fuse_bc7_encoder_tests PRIVATE fuse_cook_stubs fuse_core)
add_test(NAME fuse_bc7_encoder COMMAND fuse_bc7_encoder_tests)

# Asset plan Wave 0 (docs/plans/FUSE_ASSET_PLAN.md §6): W0.1 FMSH v2, W0.3 BCn + arrays, W0.4 KTX2.
# CPU gates, labels asset;gate.
set(_fuse_cook_bc_oracle_dir "${CMAKE_SOURCE_DIR}/Source/FUSE/Relight/mods/assets")
add_executable(fuse_asset_texture_bcn_tests test_texture_bcn.cpp)
target_link_libraries(fuse_asset_texture_bcn_tests PRIVATE fuse_cook_stubs fuse_core)
if(EXISTS "${_fuse_cook_bc_oracle_dir}/src/texture_decode.cpp")
    # Independent BCn decoder (Relight RL-3.3, checked against bcdec) as the PSNR oracle. Compiled in
    # directly so the gate has it in every tree, including those built without Relight.
    target_sources(fuse_asset_texture_bcn_tests PRIVATE
        "${_fuse_cook_bc_oracle_dir}/src/texture_decode.cpp"
        "${_fuse_cook_bc_oracle_dir}/src/texture_format.cpp")
    target_include_directories(fuse_asset_texture_bcn_tests PRIVATE "${_fuse_cook_bc_oracle_dir}/include")
    target_compile_definitions(fuse_asset_texture_bcn_tests PRIVATE FUSE_COOK_TEST_HAS_BC_ORACLE=1)
endif()
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_asset_texture_bcn_tests)
endif()
add_test(NAME fuse_asset_texture_bcn COMMAND fuse_asset_texture_bcn_tests)

add_executable(fuse_asset_ktx2_tests test_ktx2.cpp)
target_link_libraries(fuse_asset_ktx2_tests PRIVATE fuse_cook_stubs fuse_core)
if(FUSE_BUILD_TOOLS)
    # fuse_cook (Tools/FUSE) is defined after this directory; dependencies and $<TARGET_FILE> resolve
    # at generate time. The test drives `fuse_cook --texture` with .ktx2 in and out.
    add_dependencies(fuse_asset_ktx2_tests fuse_cook)
    add_test(NAME fuse_asset_ktx2 COMMAND fuse_asset_ktx2_tests "$<TARGET_FILE:fuse_cook>")
else()
    add_test(NAME fuse_asset_ktx2 COMMAND fuse_asset_ktx2_tests)
endif()

add_executable(fuse_asset_fmsh_v2_tests test_fmsh_v2.cpp)
target_link_libraries(fuse_asset_fmsh_v2_tests PRIVATE fuse_cook_stubs fuse_core)
target_compile_definitions(fuse_asset_fmsh_v2_tests PRIVATE "FUSE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
add_test(NAME fuse_asset_fmsh_v2 COMMAND fuse_asset_fmsh_v2_tests)

set_tests_properties(fuse_asset_texture_bcn fuse_asset_ktx2 fuse_asset_fmsh_v2 PROPERTIES LABELS "asset;gate" TIMEOUT 300)

# Asset plan W0.2: meshoptimizer in the cook (LOD chain, meshlets, cluster DAG, meshopt codecs). The
# meshlet / DAG suites compare against the renderer's own WP-1.2 / WP-5.2 cooks (fuse_geometry_cook).
if(TARGET fuse_meshoptimizer)
    add_executable(fuse_asset_mesh_meshopt_tests test_fmsh_meshopt.cpp)
    target_link_libraries(fuse_asset_mesh_meshopt_tests PRIVATE fuse_cook_stubs fuse_core fuse_meshoptimizer)
    target_compile_definitions(fuse_asset_mesh_meshopt_tests PRIVATE FUSE_COOK_TEST_HAS_MESHOPT=1)
    set(_fuse_mesh_meshopt_suites lod codec compat)
    if(TARGET fuse_geometry_cook AND TARGET fuse_geometry_dag)
        target_link_libraries(fuse_asset_mesh_meshopt_tests PRIVATE fuse_geometry_cook fuse_geometry_dag)
        target_compile_definitions(fuse_asset_mesh_meshopt_tests PRIVATE FUSE_COOK_TEST_HAS_GEOMETRY=1)
        list(APPEND _fuse_mesh_meshopt_suites meshlets dag)
    endif()
    foreach(_suite IN LISTS _fuse_mesh_meshopt_suites)
        add_test(NAME fuse_asset_mesh_${_suite} COMMAND fuse_asset_mesh_meshopt_tests ${_suite})
        set_tests_properties(fuse_asset_mesh_${_suite} PROPERTIES LABELS "asset;gate" TIMEOUT 600)
    endforeach()
endif()
