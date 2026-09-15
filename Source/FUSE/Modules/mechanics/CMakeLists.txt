add_library(fuse_mechanics STATIC
    src/component_interface.cpp
    src/component.cpp
    src/interactable.cpp
    src/registry.cpp
    include/fuse/mechanics/component_interface.hpp
    include/fuse/mechanics/component.hpp
    include/fuse/mechanics/interactable.hpp
    include/fuse/mechanics/registry.hpp
)

add_library(fuse::mechanics ALIAS fuse_mechanics)

target_include_directories(fuse_mechanics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_mechanics PUBLIC fuse_core)
target_compile_features(fuse_mechanics PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
