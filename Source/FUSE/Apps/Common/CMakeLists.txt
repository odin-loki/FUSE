add_library(fuse_demo_wiring STATIC demo_project_wiring.cpp)
add_library(fuse::demo_wiring ALIAS fuse_demo_wiring)

target_include_directories(fuse_demo_wiring
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(fuse_demo_wiring
    PUBLIC
        fuse_world_converter
        fuse_scene
        fuse_project
        fuse_world2d
        fuse_core
)
