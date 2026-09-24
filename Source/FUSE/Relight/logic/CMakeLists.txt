# FUSE Relight RL-3.5: Logic graphs (docs/plans/FUSE_REMIX_PORT_PLAN.md, Wave R3; port of dxvk-remix
# src/dxvk/rtx_render/graph/** at 0867d3c, MIT). Added by one line in Source/FUSE/Relight/CMakeLists.txt:
# add_subdirectory(logic), after RL-3.4 (replace).
#
# Targets
#   fuse_relight_logic       pure CPU (native and MinGW): the component runtime (68 components: Sense, Transform, Act,
#                            Constants; flexible variants), graph batches / instances / manager, the OmniGraph parser
#                            on the RL-3.1 composed stage (graph_usd_parser.hpp), option-layer actions on the RL-0.6
#                            option system, and LogicRuntime (deterministic per-frame evaluation, logic_runtime.hpp).
#   fuse_relight_logic_live  the capture-mode integration (logic_live.hpp): wraps RL-3.4's frame processor, runs the
#                            graphs of the replaced draws every frame and writes a "logic" member into the capture
#                            record's replace_frame line. tap/capture links it when it exists ($<TARGET_NAME_IF_EXISTS>).
# Tests: tests/ (rl_logic_unit; Wine: rl_logic_golden_<case>).

foreach(_dep fuse_relight_options fuse_relight_mods_usd fuse_relight_hash)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "logic needs ${_dep} (add_subdirectory(logic) after RL-3.1 mods/usd and RL-0.6 options)")
    endif()
endforeach()

add_library(fuse_relight_logic STATIC
    src/component_list.cpp
    src/components_compare.cpp
    src/components_const.cpp
    src/components_math.cpp
    src/components_options.cpp
    src/components_sense.cpp
    src/components_state.cpp
    src/graph_batch.cpp
    src/graph_types.cpp
    src/graph_usd_parser.cpp
    src/keybind.cpp
    src/logic_log.cpp
    src/logic_math.cpp
    src/logic_runtime.cpp)
target_include_directories(fuse_relight_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_logic PUBLIC fuse_relight_options fuse_relight_mods_usd PRIVATE fuse_relight_hash)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_logic)
else()
    set_target_properties(fuse_relight_logic PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

if(TARGET fuse_relight_replace_live)
    add_library(fuse_relight_logic_live STATIC src/logic_live.cpp)
    target_link_libraries(fuse_relight_logic_live PUBLIC fuse_relight_logic fuse_relight_replace_live
                          PRIVATE fuse_relight_capture_geometry)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(fuse_relight_logic_live)
    else()
        set_target_properties(fuse_relight_logic_live PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endif()

add_subdirectory(tests)
