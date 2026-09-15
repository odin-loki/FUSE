set(FUSE_PROJECT_PUBLIC_HEADERS
    include/fuse/project/manifest.hpp
    include/fuse/project/json_reader.hpp
    include/fuse/project/loader.hpp
    include/fuse/project/importer.hpp
    include/fuse/project/import_desc.hpp
    include/fuse/project/cook_manifest.hpp
    include/fuse/project/asset_graph.hpp
    include/fuse/project/asset_cooker.hpp
    include/fuse/project/cook_dependency_graph.hpp
    include/fuse/project/cook_job_graph.hpp
    include/fuse/project/cook_content_hash.hpp
    include/fuse/project/cook_cache.hpp
    include/fuse/project/import_pipeline.hpp
)

set(FUSE_PROJECT_SOURCES
    src/manifest.cpp
    src/json_reader.cpp
    src/loader.cpp
    src/importer.cpp
    src/cook_manifest.cpp
    src/asset_graph.cpp
    src/asset_cooker.cpp
    src/cook_dependency_graph.cpp
    src/cook_job_graph.cpp
    src/cook_content_hash.cpp
    src/cook_cache.cpp
    src/import_pipeline.cpp
)

add_library(fuse_project STATIC
    ${FUSE_PROJECT_SOURCES}
    ${FUSE_PROJECT_PUBLIC_HEADERS}
)

add_library(fuse::project ALIAS fuse_project)

target_include_directories(fuse_project
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_project PUBLIC fuse_hybrid fuse_core)
target_compile_features(fuse_project PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
