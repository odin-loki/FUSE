# fuse_navigation — uniform 4-connected grid navmesh

set(FUSE_NAVIGATION_PUBLIC_HEADERS
    include/fuse/navigation/grid_navmesh.hpp
)

set(FUSE_NAVIGATION_SOURCES
    src/grid_navmesh.cpp
)

add_library(fuse_navigation STATIC
    ${FUSE_NAVIGATION_SOURCES}
    ${FUSE_NAVIGATION_PUBLIC_HEADERS}
)

add_library(fuse::navigation ALIAS fuse_navigation)

target_include_directories(fuse_navigation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_navigation PUBLIC fuse_core)

set_target_properties(fuse_navigation PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_features(fuse_navigation PRIVATE cxx_std_23)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
