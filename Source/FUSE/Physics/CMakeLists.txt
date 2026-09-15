add_library(fuse_physics STATIC
    src/physics_data.cpp
    src/physics_pipeline.cpp
    src/physics_world_2d.cpp
    src/physics_world_3d.cpp
    src/broadphase/spatial_hash.cpp
    src/narrowphase/collision_dispatch.cpp
    src/narrowphase/gjk.cpp
    src/pbd_solver.cpp
    src/barnes_hut.cpp
    src/ccd.cpp
    include/fuse/physics/broadphase/spatial_hash.hpp
    include/fuse/physics/config.hpp
    include/fuse/physics/math.hpp
    include/fuse/physics/narrowphase/collision_dispatch.hpp
    include/fuse/physics/narrowphase/gjk.hpp
    include/fuse/physics/physics_data.hpp
    include/fuse/physics/physics_pipeline.hpp
    include/fuse/physics/physics_world_2d.hpp
    include/fuse/physics/physics_world_3d.hpp
    include/fuse/physics/types.hpp
    include/fuse/physics/solver/pbd_solver.hpp
    include/fuse/physics/nbody/barnes_hut.hpp
    include/fuse/physics/ccd/ccd.hpp
)

add_library(fuse::physics ALIAS fuse_physics)

target_include_directories(fuse_physics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_physics PUBLIC fuse_core)
target_compile_features(fuse_physics PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
