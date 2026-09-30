# fuse_physics — Track B physics (B4.1–B4.11 scaffolds)

add_library(fuse_physics STATIC
    # B4.1–B4.6 foundation (main)
    src/physics_data.cpp
    src/physics_pipeline.cpp
    src/physics_world_2d.cpp
    src/physics_world_3d.cpp
    src/broadphase/spatial_hash.cpp
    src/broadphase/pair_buffer.cpp
    src/narrowphase/collision_dispatch.cpp
    src/narrowphase/contact_buffer.cpp
    src/narrowphase/contact_manifold.cpp
    src/narrowphase/contact_pair.cpp
    src/narrowphase/box_box.cpp
    src/narrowphase/oriented_shapes.cpp
    src/narrowphase/friction.cpp
    src/narrowphase/gjk.cpp
    src/pbd_solver.cpp
    src/solver/solver_work_buffers.cpp
    src/solver/constraint_accumulation.cpp
    src/solver/joint_constraint.cpp
    src/solver/contact_island_graph.cpp
    src/solver/pbd_island_solve.cpp
    src/barnes_hut.cpp
    src/ccd.cpp
    src/ccd/toi_buffer.cpp
    include/fuse/physics/broadphase/spatial_hash.hpp
    include/fuse/physics/broadphase/pair_buffer.hpp
    include/fuse/physics/config.hpp
    include/fuse/physics/math.hpp
    include/fuse/physics/rotation.hpp
    include/fuse/physics/narrowphase/collision_dispatch.hpp
    include/fuse/physics/narrowphase/contact_buffer.hpp
    include/fuse/physics/narrowphase/contact_manifold.hpp
    include/fuse/physics/narrowphase/contact_pair.hpp
    include/fuse/physics/narrowphase/friction.hpp
    include/fuse/physics/narrowphase/gjk.hpp
    include/fuse/physics/narrowphase/primitive_contacts.hpp
    include/fuse/physics/physics_data.hpp
    include/fuse/physics/physics_pipeline.hpp
    include/fuse/physics/physics_world_2d.hpp
    include/fuse/physics/physics_world_3d.hpp
    include/fuse/physics/types.hpp
    include/fuse/physics/solver/distance_constraint.hpp
    include/fuse/physics/solver/pbd_solver.hpp
    include/fuse/physics/solver/solver_work_buffers.hpp
    include/fuse/physics/solver/constraint_accumulation.hpp
    include/fuse/physics/solver/contact_island_graph.hpp
    include/fuse/physics/solver/pbd_island_solve.hpp
    include/fuse/physics/nbody/barnes_hut.hpp
    include/fuse/physics/ccd/ccd.hpp
    include/fuse/physics/ccd/toi_buffer.hpp
    # B4.7–B4.11 scaffolds
    src/destruction/voxel_destruction.cpp
    src/events/collision_events.cpp
    src/phase4_test_registry.cpp
    src/physics_manager.cpp
    src/softbody/cloth_simulator.cpp
    src/softbody/particle_system.cpp
    src/spatial/svo.cpp
    include/fuse/physics/destruction/voxel_destruction.hpp
    include/fuse/physics/events/collision_events.hpp
    include/fuse/physics/phase4_test_registry.hpp
    include/fuse/physics/physics_manager.hpp
    include/fuse/physics/softbody/cloth_simulator.hpp
    include/fuse/physics/softbody/particle_system.hpp
    include/fuse/physics/spatial/svo.hpp
)

# GAP-PHYS-HULL-MESH / GAP-PHYS-CHARACTER / UNI-B4-VOX-1 (package E18): shared shape pool with convex
# hulls (quickhull), static triangle meshes (BVH), SDF samplers and voxel volumes; their CPU contacts;
# shape casts, the character controller and the cooked .fusecol collision asset.
target_sources(fuse_physics PRIVATE
    src/shapes/convex_hull.cpp
    src/shapes/tri_mesh.cpp
    src/shapes/sdf_sampler.cpp
    src/shapes/shape_pool.cpp
    src/narrowphase/contact_cluster.cpp
    src/narrowphase/hull_contacts.cpp
    src/narrowphase/mesh_contacts.cpp
    src/narrowphase/field_contacts.cpp
    src/narrowphase/pooled_dispatch.cpp
    src/queries/shape_queries.cpp
    src/character/character_controller.cpp
    src/assets/collision_asset.cpp
    include/fuse/physics/shapes/convex_hull.hpp
    include/fuse/physics/shapes/tri_mesh.hpp
    include/fuse/physics/shapes/sdf_sampler.hpp
    include/fuse/physics/shapes/shape_pool.hpp
    include/fuse/physics/narrowphase/contact_cluster.hpp
    include/fuse/physics/narrowphase/hull_contacts.hpp
    include/fuse/physics/narrowphase/mesh_contacts.hpp
    include/fuse/physics/narrowphase/field_contacts.hpp
    include/fuse/physics/queries/shape_queries.hpp
    include/fuse/physics/character/character_controller.hpp
    include/fuse/physics/assets/collision_asset.hpp
)

add_library(fuse::physics ALIAS fuse_physics)

target_include_directories(fuse_physics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# UNI-B4-VOX-1: VoxelVolume stores its voxels in the Scene SVO (fuse_svo, the one SVO type shared with
# fuse_scene); the library is defined by the Scene fragment so Physics does not depend on fuse_scene.
include(${CMAKE_CURRENT_SOURCE_DIR}/../Scene/cmake/fuse_svo.cmake)
target_link_libraries(fuse_physics PUBLIC fuse_core fuse_ecs fuse_svo)

# Single-source compute kernels (docs/compute-kernels.md): broadphase (keys, radix sort, pair
# emission), narrowphase and the colored constraint solve run as kernel::launch dispatches on the CPU
# backends; the broadphase sort also has a CUDA wrapper when the toolkit is found.
target_sources(fuse_physics PRIVATE
    src/broadphase/broadphase_kernels.cpp
    src/narrowphase/narrowphase_kernels.cpp
    src/solver/constraint_coloring.cpp
    include/fuse/physics/broadphase/broadphase_kernel.hpp
    include/fuse/physics/broadphase/broadphase_kernels.hpp
    include/fuse/physics/broadphase/radix_sort_launch.hpp
    include/fuse/physics/narrowphase/narrowphase_kernels.hpp
    include/fuse/physics/solver/constraint_coloring.hpp
)

# Resident pipeline (fuse/physics/resident/): broadphase -> narrowphase -> coloured solver iterations on
# buffers that stay in one memory space; the same launch sequence runs on the CPU backends (host memory,
# the reference the CPU gates check) and on CUDA (device memory, one stream, copies only at the edges).
target_sources(fuse_physics PRIVATE
    src/resident/resident_physics.cpp
    include/fuse/physics/resident/resident_cuda_backend.hpp
    include/fuse/physics/resident/resident_kernels.hpp
    include/fuse/physics/resident/resident_physics.hpp
)
if(FUSE_CUDA_BACKEND)
    target_sources(fuse_physics PRIVATE kernels/physics_broadphase.cu kernels/physics_resident.cu)
    set_target_properties(fuse_physics PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    # No FMA contraction: device floats then match the x86-64 CpuReference bit for bit (the resident
    # device gate compares them), and a rotated box lands in the same hash cells on both sides.
    set_source_files_properties(kernels/physics_broadphase.cu kernels/physics_resident.cu
        PROPERTIES COMPILE_OPTIONS "--fmad=false")
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
