add_library(fuse_ai STATIC
    src/agent_snapshot.cpp
    src/behavior_tree.cpp
    src/blackboard.cpp
    src/behavior_runtime.cpp
    src/node_registry.cpp
    src/spatial_query.cpp
    src/tree_loader.cpp
    include/fuse/ai/agent_snapshot.hpp
    include/fuse/ai/blackboard.hpp
    include/fuse/ai/behavior_tree.hpp
    include/fuse/ai/behavior_runtime.hpp
    include/fuse/ai/node_registry.hpp
    include/fuse/ai/spatial_query.hpp
    include/fuse/ai/tree_loader.hpp
    include/fuse/ai/uaisk_template_hooks.hpp
)

add_library(fuse::ai ALIAS fuse_ai)

target_include_directories(fuse_ai
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_ai PUBLIC fuse_core fuse_world2d)
target_compile_features(fuse_ai PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
