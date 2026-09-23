add_library(fuse_cook_stubs STATIC
    src/bc7_encoder.cpp
    src/cook_stub_writer.cpp
    src/mesh_cook.cpp
    src/texture_cook.cpp
    src/cook_stb_image.cpp
    src/ispc_texcomp_hook.cpp
    include/fuse/cook/bc7_encoder.hpp
    include/fuse/cook/cook_stub_writer.hpp
    include/fuse/cook/mesh_cook.hpp
    include/fuse/cook/texture_cook.hpp
    include/fuse/cook/ispc_texcomp_hook.hpp
)

add_library(fuse::cook_stubs ALIAS fuse_cook_stubs)

target_include_directories(fuse_cook_stubs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_cook_stubs PUBLIC fuse_core)
target_compile_features(fuse_cook_stubs PUBLIC cxx_std_17)
target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_INHOUSE_BC7_ENCODER=1)

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
elseif(EXISTS "${CMAKE_SOURCE_DIR}/Engine/lib/assimp/CMakeLists.txt")
    set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # assimp's MSVC branch appends /Zi to CMAKE_CXX_FLAGS_RELEASE when ASSIMP_INSTALL_PDB is on
    # (and to CMAKE_CXX_FLAGS_DEBUG unconditionally). /Zi makes every cl.exe write one shared
    # assimp-vc143-mt.pdb through mspdbsrv, which fails under sccache with C1041/C1090 even with
    # /FS. We never install assimp, so drop its PDB install; the Debug /Zi is overridden below.
    set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/Engine/lib/assimp" "${CMAKE_BINARY_DIR}/fuse_vendored_assimp" EXCLUDE_FROM_ALL)
    if(TARGET assimp AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # Object-embedded debug info (/Z7) like the rest of the tree (see the fuse-msvc presets):
        # target options come after CMAKE_CXX_FLAGS_<CONFIG>, so this wins over assimp's /Zi
        # (cl prints D9025 "overriding '/Zi' with '/Z7'", a command-line note, not an error).
        # clang-cl already treats /Zi as /Z7, so it is left alone.
        target_compile_options(assimp PRIVATE $<$<CONFIG:Debug>:/Z7>)
    endif()
    if(TARGET assimp)
        target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_ASSIMP=1)
        target_link_libraries(fuse_cook_stubs PRIVATE assimp)
    endif()
endif()

find_package(Vorbis QUIET)
find_package(Ogg QUIET)
if(Vorbis_FOUND)
    target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_OGG_VORBIS=1)
    target_link_libraries(fuse_cook_stubs PRIVATE Vorbis::vorbisenc Vorbis::vorbis)
    if(Ogg_FOUND)
        target_link_libraries(fuse_cook_stubs PRIVATE Ogg::ogg)
    endif()
else()
    find_path(FUSE_VORBIS_INCLUDE_DIR NAMES vorbis/vorbisenc.h)
    find_library(FUSE_VORBISENC_LIBRARY NAMES vorbisenc)
    find_library(FUSE_VORBIS_LIBRARY NAMES vorbis)
    find_library(FUSE_OGG_LIBRARY NAMES ogg)
    if(FUSE_VORBIS_INCLUDE_DIR AND FUSE_VORBISENC_LIBRARY AND FUSE_VORBIS_LIBRARY)
        target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_OGG_VORBIS=1)
        target_include_directories(fuse_cook_stubs PRIVATE "${FUSE_VORBIS_INCLUDE_DIR}")
        target_link_libraries(fuse_cook_stubs PRIVATE "${FUSE_VORBISENC_LIBRARY}" "${FUSE_VORBIS_LIBRARY}")
        if(FUSE_OGG_LIBRARY)
            target_link_libraries(fuse_cook_stubs PRIVATE "${FUSE_OGG_LIBRARY}")
        endif()
    endif()
endif()

find_path(FUSE_ISPC_TEXCOMP_INCLUDE NAMES ispc_texcomp.h
    PATHS "${CMAKE_SOURCE_DIR}/third_party/ispc_texcomp")
if(FUSE_ISPC_TEXCOMP_INCLUDE)
    target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_ISPC_TEXCOMP=1)
    target_include_directories(fuse_cook_stubs PRIVATE "${FUSE_ISPC_TEXCOMP_INCLUDE}")
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
