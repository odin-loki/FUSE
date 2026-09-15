add_library(fuse_adventure STATIC
    src/inventory.cpp
    src/inventory_component.cpp
    src/interactable.cpp
    src/interaction.cpp
    src/pickup_interactable.cpp
    src/adventure_interactable_component.cpp
    src/mechanics_bridge.cpp
    src/puzzle_gate.cpp
    include/fuse/adventure/item_id.hpp
    include/fuse/adventure/inventory.hpp
    include/fuse/adventure/inventory_component.hpp
    include/fuse/adventure/interactable.hpp
    include/fuse/adventure/interaction.hpp
    include/fuse/adventure/pickup_interactable.hpp
    include/fuse/adventure/adventure_interactable_component.hpp
    include/fuse/adventure/mechanics_bridge.hpp
    include/fuse/adventure/puzzle_gate.hpp
)

add_library(fuse::adventure ALIAS fuse_adventure)

target_include_directories(fuse_adventure
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_adventure PUBLIC fuse_core fuse_mechanics)
target_compile_features(fuse_adventure PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
