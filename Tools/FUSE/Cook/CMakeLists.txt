add_library(fuse_cook_stubs STATIC
    src/cook_stub_writer.cpp
    src/cook_stb_image.cpp
    include/fuse/cook/cook_stub_writer.hpp
)

add_library(fuse::cook_stubs ALIAS fuse_cook_stubs)

target_include_directories(fuse_cook_stubs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_cook_stubs PUBLIC fuse_core)
target_compile_features(fuse_cook_stubs PUBLIC cxx_std_17)

set(FUSE_ENGINE_STB_DIR "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/loaders/stb")
if(EXISTS "${FUSE_ENGINE_STB_DIR}/stb_image.h")
    target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_STB_IMAGE=1)
    target_include_directories(fuse_cook_stubs PRIVATE "${FUSE_ENGINE_STB_DIR}")
endif()

find_package(assimp QUIET)
if(assimp_FOUND OR ASSIMP_FOUND)
    target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_ASSIMP=1)
    if(TARGET assimp::assimp)
        target_link_libraries(fuse_cook_stubs PRIVATE assimp::assimp)
    endif()
endif()

find_package(Vorbis QUIET)
if(Vorbis_FOUND)
    target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_OGG_VORBIS=1)
    target_link_libraries(fuse_cook_stubs PRIVATE Vorbis::vorbisenc Vorbis::vorbis)
endif()
