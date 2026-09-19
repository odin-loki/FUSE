add_library(fuse_cook_stubs STATIC
    src/cook_stub_writer.cpp
    include/fuse/cook/cook_stub_writer.hpp
)

add_library(fuse::cook_stubs ALIAS fuse_cook_stubs)

target_include_directories(fuse_cook_stubs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_cook_stubs PUBLIC fuse_core)
target_compile_features(fuse_cook_stubs PUBLIC cxx_std_17)
