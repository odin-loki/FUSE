add_library(fuse_adventure STATIC
    src/inventory.cpp
    src/inventory_component.cpp
    src/interactable.cpp
    src/interaction.cpp
    src/interaction_queue.cpp
    src/pickup_interactable.cpp
    src/adventure_interactable_component.cpp
    src/examine_interactable.cpp
    src/conversation_interactable.cpp
    src/door_interactable.cpp
    src/weapon_pickup_interactable.cpp
    src/hud_prompt_interactable.cpp
    src/mechanics_bridge.cpp
    src/puzzle_gate.cpp
    src/outpost_loader.cpp
    src/outpost_spawn.cpp
    src/conversation_script_vm.cpp
    include/fuse/adventure/item_id.hpp
    include/fuse/adventure/inventory.hpp
    include/fuse/adventure/inventory_component.hpp
    include/fuse/adventure/interactable.hpp
    include/fuse/adventure/interaction.hpp
    include/fuse/adventure/interaction_queue.hpp
    include/fuse/adventure/pickup_interactable.hpp
    include/fuse/adventure/adventure_interactable_component.hpp
    include/fuse/adventure/examine_interactable.hpp
    include/fuse/adventure/conversation_interactable.hpp
    include/fuse/adventure/door_interactable.hpp
    include/fuse/adventure/weapon_pickup_interactable.hpp
    include/fuse/adventure/hud_prompt_interactable.hpp
    include/fuse/adventure/mechanics_bridge.hpp
    include/fuse/adventure/puzzle_gate.hpp
    include/fuse/adventure/outpost_loader.hpp
    include/fuse/adventure/outpost_spawn.hpp
    include/fuse/adventure/conversation_script_vm.hpp
)

add_library(fuse::adventure ALIAS fuse_adventure)

target_include_directories(fuse_adventure
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_adventure PUBLIC fuse_core fuse_mechanics fuse_world3d)
target_compile_features(fuse_adventure PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
