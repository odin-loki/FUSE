# WP-5.2 (docs/unification/RENDERER-EXECUTION.md): cluster DAG + LOD over the WP-1.2 meshlets.
# Owned by WP-5.2; other packages must not edit this file. Included after rp_wp12.cmake
# (needs fuse_geometry and fuse_meshoptimizer).
#
#   fuse_geometry_dag   STATIC  DAG builder (meshoptimizer partition + simplify), FMLT 1.1 DAG chunks,
#                               single-source LOD cut kernel "geometry_dag_cut" (CPU only, stub-safe)
#   fuse_rp_cluster_dag_{build,crack,cut,format,determinism}   ctest gates, labels gate;renderer

set(_fuse_dag_dir "${CMAKE_CURRENT_LIST_DIR}/../geometry/dag")

add_library(fuse_geometry_dag STATIC
    ${_fuse_dag_dir}/src/cluster_dag_builder.cpp
    ${_fuse_dag_dir}/src/cluster_dag_format.cpp
    ${_fuse_dag_dir}/src/fmlt_chunks.cpp
    ${_fuse_dag_dir}/include/fuse/renderer/geometry/dag/cluster_dag_types.hpp
    ${_fuse_dag_dir}/include/fuse/renderer/geometry/dag/cluster_dag.hpp
    ${_fuse_dag_dir}/include/fuse/renderer/geometry/dag/dag_cut_kernel.hpp
    ${_fuse_dag_dir}/include/fuse/renderer/geometry/dag/fmlt_chunks.hpp
)
target_include_directories(fuse_geometry_dag PUBLIC ${_fuse_dag_dir}/include)
target_link_libraries(fuse_geometry_dag PUBLIC fuse_geometry PRIVATE fuse_meshoptimizer)
fuse_apply_cxx23(fuse_geometry_dag)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_rp_cluster_dag_tests ${_fuse_dag_dir}/tests/test_rp_cluster_dag.cpp)
    target_link_libraries(fuse_rp_cluster_dag_tests PRIVATE fuse_geometry_dag)
    fuse_apply_cxx23(fuse_rp_cluster_dag_tests)
    foreach(_suite build crack cut format determinism)
        add_test(NAME fuse_rp_cluster_dag_${_suite} COMMAND fuse_rp_cluster_dag_tests ${_suite})
        set_tests_properties(fuse_rp_cluster_dag_${_suite} PROPERTIES LABELS "gate;renderer" TIMEOUT 900)
    endforeach()
endif()
