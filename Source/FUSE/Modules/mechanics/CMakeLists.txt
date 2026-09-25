add_library(fuse_mechanics STATIC
    src/action_stub.cpp
    src/component_interface.cpp
    src/component.cpp
    src/health_component.cpp
    src/trigger_zone.cpp
    src/console_method_component.cpp
    src/polyhedron_trigger.cpp
    src/physics_trigger_bridge.cpp
    src/broadphase_trigger_sync.cpp
    src/broadphase_proxy_filter.cpp
    src/physics_broadphase_bridge.cpp
    src/counter_component.cpp
    src/message_component.cpp
    src/delay_component.cpp
    src/rotate_component.cpp
    src/toggle_component.cpp
    src/switch_component.cpp
    src/spawn_component.cpp
    src/destroy_component.cpp
    src/area_component.cpp
    src/sound_component.cpp
    src/light_component.cpp
    src/move_component.cpp
    src/camera_component.cpp
    src/follow_component.cpp
    src/path_component.cpp
    src/timer_component.cpp
    src/animate_component.cpp
    src/waypoint_component.cpp
    src/look_at_component.cpp
    src/radio_component.cpp
    src/bt_dbvt_bridge.cpp
    src/broadphase_world_stub.cpp
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
    include/fuse/mechanics/broadphase_trigger_sync.hpp
    include/fuse/mechanics/broadphase_proxy_filter.hpp
    include/fuse/mechanics/physics_broadphase_bridge.hpp
    include/fuse/mechanics/counter_component.hpp
    include/fuse/mechanics/message_component.hpp
    include/fuse/mechanics/delay_component.hpp
    include/fuse/mechanics/rotate_component.hpp
    include/fuse/mechanics/toggle_component.hpp
    include/fuse/mechanics/switch_component.hpp
    include/fuse/mechanics/spawn_component.hpp
    include/fuse/mechanics/destroy_component.hpp
    include/fuse/mechanics/area_component.hpp
    include/fuse/mechanics/sound_component.hpp
    include/fuse/mechanics/light_component.hpp
    include/fuse/mechanics/move_component.hpp
    include/fuse/mechanics/camera_component.hpp
    include/fuse/mechanics/follow_component.hpp
    include/fuse/mechanics/path_component.hpp
    include/fuse/mechanics/timer_component.hpp
    include/fuse/mechanics/animate_component.hpp
    include/fuse/mechanics/waypoint_component.hpp
    include/fuse/mechanics/look_at_component.hpp
    include/fuse/mechanics/radio_component.hpp
    include/fuse/mechanics/bt_dbvt_bridge.hpp
    include/fuse/mechanics/broadphase_world_stub.hpp
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

option(FUSE_BUILD_BULLET "Link Bullet btDbvt for mechanics broadphase deepen" OFF)
if(FUSE_BUILD_BULLET)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${FUSE_VENDOR_DIR}/bullet ${CMAKE_BINARY_DIR}/bullet EXCLUDE_FROM_ALL)
    target_compile_definitions(fuse_mechanics PUBLIC FUSE_HAS_BULLET=1 FUSE_BUILD_BULLET=1)
    target_include_directories(fuse_mechanics PRIVATE ${FUSE_VENDOR_DIR}/bullet/src)
    target_link_libraries(fuse_mechanics PRIVATE BulletCollision LinearMath)
else()
    target_compile_definitions(fuse_mechanics PUBLIC FUSE_HAS_BULLET=0)
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
