# fuse_net — Track B networking scaffolding (B7.4)

set(FUSE_NET_PUBLIC_HEADERS
    include/fuse/net/transport.hpp
    include/fuse/net/serializer.hpp
    include/fuse/net/game_state.hpp
    include/fuse/net/checksum.hpp
    include/fuse/net/input_history.hpp
    include/fuse/net/reconcile.hpp
    include/fuse/net/rollback_window.hpp
    include/fuse/net/rollback.hpp
    include/fuse/net/rollback_buffer.hpp
    include/fuse/net/snapshot_delta.hpp
    include/fuse/net/state_sync.hpp
    include/fuse/net/interest_management.hpp
)

set(FUSE_NET_SOURCES
    src/transport.cpp
    src/serializer.cpp
    src/checksum.cpp
    src/input_history.cpp
    src/reconcile.cpp
    src/rollback_window.cpp
    src/rollback.cpp
    src/rollback_buffer.cpp
    src/snapshot_delta.cpp
    src/state_sync.cpp
    src/interest_management.cpp
)

add_library(fuse_net STATIC
    ${FUSE_NET_SOURCES}
    ${FUSE_NET_PUBLIC_HEADERS}
)

add_library(fuse::net ALIAS fuse_net)

target_include_directories(fuse_net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_net PUBLIC fuse_ecs fuse_core)

# ENet backend — vendored ENet 1.3.7 (MIT) under Engine/lib/enet (target fuse_enet).
option(FUSE_NET_WITH_ENET "Build the ENet UDP transport backend for fuse_net" ON)
set(_fuse_enet_dir "${FUSE_VENDOR_DIR}/enet")
set(FUSE_NET_HAS_ENET OFF)
if(FUSE_NET_WITH_ENET AND EXISTS "${_fuse_enet_dir}/include/enet/enet.h")
    if(NOT TARGET fuse_enet)
        add_subdirectory("${_fuse_enet_dir}" "${CMAKE_BINARY_DIR}/fuse_vendor/enet" EXCLUDE_FROM_ALL)
    endif()
    target_link_libraries(fuse_net PRIVATE fuse_enet)
    target_compile_definitions(fuse_net PUBLIC FUSE_NET_HAS_ENET=1)
    set(FUSE_NET_HAS_ENET ON)
    message(STATUS "FUSE: fuse_net ENet backend enabled (${_fuse_enet_dir})")
else()
    message(STATUS "FUSE: fuse_net ENet backend disabled — ENetTransport is a stub")
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
