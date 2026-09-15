# WP-08 thin start — editor API boundary (no Qt in fuse_core).

add_library(fuse_editor_api STATIC
    src/command_queue.cpp
    include/fuse/editor/command_queue.hpp
)

add_library(fuse::editor_api ALIAS fuse_editor_api)

target_include_directories(fuse_editor_api
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_editor_api PUBLIC fuse_core)
target_compile_features(fuse_editor_api PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
