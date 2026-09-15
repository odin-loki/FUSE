set(FUSE_ECS_PUBLIC_HEADERS
    include/fuse/ecs/entity.hpp
    include/fuse/ecs/component.hpp
    include/fuse/ecs/archetype.hpp
    include/fuse/ecs/registry.hpp
    include/fuse/ecs/math/vec.hpp
    include/fuse/ecs/components/transform.hpp
    include/fuse/ecs/components/mesh.hpp
    include/fuse/ecs/components/sdf_object.hpp
    include/fuse/ecs/components/rigidbody.hpp
    include/fuse/ecs/components/camera.hpp
    include/fuse/ecs/components/light.hpp
    include/fuse/ecs/components/tags.hpp
)

set(FUSE_ECS_SOURCES
    src/archetype.cpp
    src/registry.cpp
)

add_library(fuse_ecs STATIC
    ${FUSE_ECS_SOURCES}
    ${FUSE_ECS_PUBLIC_HEADERS}
)

add_library(fuse::ecs ALIAS fuse_ecs)

target_include_directories(fuse_ecs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(fuse_ecs PUBLIC fuse_core)
target_compile_features(fuse_ecs PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
