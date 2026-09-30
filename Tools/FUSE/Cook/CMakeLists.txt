add_library(fuse_cook_stubs STATIC
    src/bc7_encoder.cpp
    src/bcn_encoder.cpp
    src/ktx2.cpp
    src/cook_stub_writer.cpp
    src/mesh_cook.cpp
    src/mesh_cook_meshopt.cpp
    src/mesh_cook_meshopt.hpp
    src/texture_cook.cpp
    src/cook_stb_image.cpp
    src/ispc_texcomp_hook.cpp
    src/audio_cook.cpp
    include/fuse/cook/audio_cook.hpp
    include/fuse/cook/bc7_encoder.hpp
    include/fuse/cook/bcn_encoder.hpp
    include/fuse/cook/ktx2.hpp
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

target_link_libraries(fuse_cook_stubs PUBLIC fuse_core fuse_asset)  # UNI-U7-ASSET-1: readers live in fuse_asset
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_cook_stubs)
endif()
target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_INHOUSE_BC7_ENCODER=1)

if(EXISTS "${FUSE_STB_DIR}/stb_image.h")
    target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_STB_IMAGE=1)
    fuse_stb_include(fuse_cook_stubs PRIVATE)
endif()

# Asset plan W0.2: meshoptimizer (LODs, vertex / index codecs) and the renderer's WP-1.2 meshlet +
# WP-5.2 cluster DAG builders (Source/FUSE/Renderer/cmake/rp_wp12.cmake, rp_wp52.cmake; the Renderer is
# added before this directory). Linked, not duplicated. Without them the W0.2 steps report unavailable.
if(TARGET fuse_meshoptimizer)
    target_link_libraries(fuse_cook_stubs PRIVATE fuse_meshoptimizer)
    target_compile_definitions(fuse_cook_stubs PRIVATE FUSE_COOK_HAS_MESHOPTIMIZER=1)
endif()
if(TARGET fuse_geometry_dag)
    target_link_libraries(fuse_cook_stubs PRIVATE fuse_geometry_dag)
    target_compile_definitions(fuse_cook_stubs PRIVATE FUSE_COOK_HAS_GEOMETRY_DAG=1)
endif()
# RE-P1-7: the WP-5.3 page builder (Source/FUSE/Renderer/cmake/rp_wp53.cmake) for the `.fusepages` cook.
if(TARGET fuse_geometry_streaming)
    target_link_libraries(fuse_cook_stubs PRIVATE fuse_geometry_streaming)
    target_compile_definitions(fuse_cook_stubs PRIVATE FUSE_COOK_HAS_GEOMETRY_STREAMING=1)
endif()

find_package(assimp QUIET)
if(assimp_FOUND OR ASSIMP_FOUND)
    target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_ASSIMP=1)
    if(TARGET assimp::assimp)
        target_link_libraries(fuse_cook_stubs PRIVATE assimp::assimp)
    endif()
elseif(EXISTS "${FUSE_VENDOR_DIR}/assimp/CMakeLists.txt")
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
    add_subdirectory("${FUSE_VENDOR_DIR}/assimp" "${CMAKE_BINARY_DIR}/fuse_vendored_assimp" EXCLUDE_FROM_ALL)
    if(TARGET assimp AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # Object-embedded debug info (/Z7) like the rest of the tree (see the fuse-msvc presets):
        # target options come after CMAKE_CXX_FLAGS_<CONFIG>, so this wins over assimp's /Zi
        # (cl prints D9025 "overriding '/Zi' with '/Z7'", a command-line note, not an error).
        # clang-cl already treats /Zi as /Z7, so it is left alone.
        target_compile_options(assimp PRIVATE $<$<CONFIG:Debug>:/Z7>)
        # assimp's Debug flags still carry /Zi ahead of the /Z7 above; cl.exe honours the last one,
        # but sccache parses the /Zi and then fails looking for assimp.pdb. Compile assimp uncached.
        set_target_properties(assimp PROPERTIES CXX_COMPILER_LAUNCHER "" C_COMPILER_LAUNCHER "")
    endif()
    if(TARGET assimp)
        target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_ASSIMP=1)
        target_link_libraries(fuse_cook_stubs PRIVATE assimp)
    endif()
endif()

# MP-B7.9-AUDIO-IMPORT / AP-W8.3: the audio cook chain (src/audio_cook.cpp) decodes FLAC / Ogg Vorbis and
# encodes quality-VBR Ogg Vorbis through the vendored xiph libraries (cmake/FuseXiph.cmake, built by
# Source/FUSE/Audio, which is added before this directory). Never a system libvorbis.
if(NOT TARGET fuse_xiph_vorbisenc AND EXISTS "${CMAKE_SOURCE_DIR}/cmake/FuseXiph.cmake")
    include("${CMAKE_SOURCE_DIR}/cmake/FuseXiph.cmake")
endif()
if(TARGET fuse_xiph_vorbisenc AND TARGET fuse_xiph_vorbisfile)
    target_link_libraries(fuse_cook_stubs PRIVATE fuse_xiph_vorbisfile fuse_xiph_vorbisenc fuse_xiph_vorbis fuse_xiph_ogg)
    target_compile_definitions(fuse_cook_stubs PRIVATE FUSE_COOK_HAS_VORBIS=1)
endif()
if(TARGET fuse_xiph_flac)
    target_link_libraries(fuse_cook_stubs PRIVATE fuse_xiph_flac)
    target_compile_definitions(fuse_cook_stubs PRIVATE FUSE_COOK_HAS_FLAC=1)
endif()

find_path(FUSE_ISPC_TEXCOMP_INCLUDE NAMES ispc_texcomp.h
    PATHS "${CMAKE_SOURCE_DIR}/vendor/ispc_texcomp")
if(FUSE_ISPC_TEXCOMP_INCLUDE)
    target_compile_definitions(fuse_cook_stubs PUBLIC FUSE_HAS_ISPC_TEXCOMP=1)
    target_include_directories(fuse_cook_stubs PRIVATE "${FUSE_ISPC_TEXCOMP_INCLUDE}")
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
