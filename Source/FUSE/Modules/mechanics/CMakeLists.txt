add_library(fuse_mechanics STATIC
    src/action_stub.cpp
    src/component_interface.cpp
    src/component.cpp
    src/health_component.cpp
    src/trigger_zone.cpp
    src/console_method_component.cpp
    src/polyhedron_trigger.cpp
    src/physics_trigger_bridge.cpp
    src/delay_component.cpp
    src/rotate_component.cpp
    src/toggle_component.cpp
    src/inventory_provider.cpp
    src/interact_action.cpp
    src/interactable.cpp
    src/registry.cpp
    include/fuse/mechanics/action_stub.hpp
    include/fuse/mechanics/component_interface.hpp
    include/fuse/mechanics/component.hpp
    include/fuse/mechanics/health_component.hpp
    include/fuse/mechanics/trigger_zone.hpp
    include/fuse/mechanics/console_method_component.hpp
    include/fuse/mechanics/polyhedron_trigger.hpp
    include/fuse/mechanics/physics_trigger_bridge.hpp
    include/fuse/mechanics/delay_component.hpp
    include/fuse/mechanics/rotate_component.hpp
    include/fuse/mechanics/toggle_component.hpp
    include/fuse/mechanics/inventory_provider.hpp
    include/fuse/mechanics/interact_action.hpp
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
