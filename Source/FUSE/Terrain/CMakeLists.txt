# fuse_terrain — Track B7.5 terrain scaffolding (heightfield + chunk LOD grid)

set(FUSE_TERRAIN_PUBLIC_HEADERS
    include/fuse/terrain/math.hpp
    include/fuse/terrain/terrain_desc.hpp
    include/fuse/terrain/heightfield.hpp
    include/fuse/terrain/lod.hpp
    include/fuse/terrain/lod_residency_budget.hpp
    include/fuse/terrain/lod_residency_queue.hpp
    include/fuse/terrain/lod_residency_set.hpp
    include/fuse/terrain/chunk_grid.hpp
    include/fuse/terrain/chunk_mesh.hpp
    include/fuse/terrain/queries.hpp
    include/fuse/terrain/terrain.hpp
    include/fuse/terrain/terrain_caves.hpp
    include/fuse/terrain/terrain_noise.hpp
)

set(FUSE_TERRAIN_SOURCES
    src/heightfield.cpp
    src/lod.cpp
    src/lod_residency_queue.cpp
    src/chunk_grid.cpp
    src/chunk_mesh.cpp
    src/queries.cpp
    src/terrain.cpp
    src/terrain_noise.cpp
)

add_library(fuse_terrain STATIC
    ${FUSE_TERRAIN_SOURCES}
    ${FUSE_TERRAIN_PUBLIC_HEADERS}
)

add_library(fuse::terrain ALIAS fuse_terrain)

target_include_directories(fuse_terrain
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_terrain PUBLIC fuse_core)

# Underground caves live in the B3.5 sparse voxel octree (fuse_scene, built with FUSE_BUILD_PROJECT).
# Without it TerrainCaves compiles to an always-empty volume and ray casts use the heightfield only.
if(FUSE_BUILD_PROJECT)
    target_sources(fuse_terrain PRIVATE src/terrain_caves.cpp)
    target_link_libraries(fuse_terrain PUBLIC fuse_scene)
    target_compile_definitions(fuse_terrain PUBLIC FUSE_TERRAIN_HAS_SVO=1)
endif()
target_compile_features(fuse_terrain PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
