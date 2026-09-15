# fuse_world_partition — Track B7.6 world partition & streaming stubs

set(FUSE_WORLD_PARTITION_PUBLIC_HEADERS
    include/fuse/world_partition/grid_cell.hpp
    include/fuse/world_partition/residency_set.hpp
    include/fuse/world_partition/streaming_budget.hpp
    include/fuse/world_partition/streaming_request_queue.hpp
    include/fuse/world_partition/streaming_volume.hpp
    include/fuse/world_partition/world_partition.hpp
)

set(FUSE_WORLD_PARTITION_SOURCES
    src/streaming_request_queue.cpp
    src/world_partition.cpp
)

add_library(fuse_world_partition STATIC
    ${FUSE_WORLD_PARTITION_SOURCES}
    ${FUSE_WORLD_PARTITION_PUBLIC_HEADERS}
)

add_library(fuse::world_partition ALIAS fuse_world_partition)

target_include_directories(fuse_world_partition
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_world_partition PUBLIC fuse_ecs fuse_core)
target_compile_features(fuse_world_partition PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
