# FUSE Relight RL-2.1: generated bridge command schema. Added from ../ipc/CMakeLists.txt.
# commands.table is the single source of the command ids and wire shapes; gen_commands.cmake (pure
# CMake, no Python or host compiler needed in cross builds) writes the codec at build time.

set(_rl_schema_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(_rl_schema_outputs
    "${_rl_schema_gen}/commands.gen.cpp"
    "${_rl_schema_gen}/fuse/relight/bridge/schema/commands.gen.hpp"
    "${_rl_schema_gen}/fuse/relight/bridge/schema/commands_random.gen.hpp")
add_custom_command(
    OUTPUT ${_rl_schema_outputs}
    COMMAND "${CMAKE_COMMAND}" "-DTABLE=${CMAKE_CURRENT_SOURCE_DIR}/commands.table" "-DOUT_DIR=${_rl_schema_gen}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/gen_commands.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/commands.table" "${CMAKE_CURRENT_SOURCE_DIR}/gen_commands.cmake"
    COMMENT "Generating the FUSE Relight bridge command schema"
    VERBATIM)
add_custom_target(fuse_relight_bridge_schema_gen DEPENDS ${_rl_schema_outputs})

add_library(fuse_relight_bridge_schema STATIC "${_rl_schema_gen}/commands.gen.cpp")
target_include_directories(fuse_relight_bridge_schema PUBLIC "${_rl_schema_gen}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_bridge_schema PUBLIC fuse_relight_bridge_ipc_core)
add_dependencies(fuse_relight_bridge_schema fuse_relight_bridge_schema_gen)
