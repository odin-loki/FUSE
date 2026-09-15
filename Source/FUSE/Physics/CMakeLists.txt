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
    src/narrowphase/box_box.cpp
    src/narrowphase/friction.cpp
    src/narrowphase/gjk.cpp
    src/pbd_solver.cpp
    src/solver/solver_work_buffers.cpp
    src/solver/constraint_accumulation.cpp
    src/solver/contact_island_graph.cpp
    src/barnes_hut.cpp
    src/ccd.cpp
    src/ccd/toi_buffer.cpp
    include/fuse/physics/broadphase/spatial_hash.hpp
    include/fuse/physics/broadphase/pair_buffer.hpp
    include/fuse/physics/config.hpp
    include/fuse/physics/math.hpp
    include/fuse/physics/narrowphase/collision_dispatch.hpp
    include/fuse/physics/narrowphase/contact_buffer.hpp
    include/fuse/physics/narrowphase/contact_manifold.hpp
    include/fuse/physics/narrowphase/friction.hpp
    include/fuse/physics/narrowphase/gjk.hpp
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

add_library(fuse::physics ALIAS fuse_physics)

target_include_directories(fuse_physics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_physics PUBLIC fuse_core fuse_ecs)
target_compile_features(fuse_physics PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
