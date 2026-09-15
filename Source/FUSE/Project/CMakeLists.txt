set(FUSE_PROJECT_PUBLIC_HEADERS
    include/fuse/project/manifest.hpp
    include/fuse/project/json_reader.hpp
    include/fuse/project/loader.hpp
    include/fuse/project/importer.hpp
)

set(FUSE_PROJECT_SOURCES
    src/manifest.cpp
    src/json_reader.cpp
    src/loader.cpp
    src/importer.cpp
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
