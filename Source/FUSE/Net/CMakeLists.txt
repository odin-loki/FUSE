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
target_compile_features(fuse_net PUBLIC cxx_std_17)

# ENet backend — compiled from the in-tree ENet 1.3.x sources (MIT) when present.
option(FUSE_NET_WITH_ENET "Build the ENet UDP transport backend for fuse_net" ON)
set(FUSE_NET_ENET_DIR "${CMAKE_SOURCE_DIR}/Engine/lib/bullet/examples/ThirdPartyLibs/enet"
    CACHE PATH "ENet source directory (host.c, protocol.c, include/enet/enet.h)")
set(FUSE_NET_HAS_ENET OFF)
if(FUSE_NET_WITH_ENET AND EXISTS "${FUSE_NET_ENET_DIR}/include/enet/enet.h")
    get_property(_fuse_net_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    if(NOT "C" IN_LIST _fuse_net_languages)
        enable_language(C)
    endif()
    set(_fuse_enet_sources
        ${FUSE_NET_ENET_DIR}/callbacks.c
        ${FUSE_NET_ENET_DIR}/compress.c
        ${FUSE_NET_ENET_DIR}/host.c
        ${FUSE_NET_ENET_DIR}/list.c
        ${FUSE_NET_ENET_DIR}/packet.c
        ${FUSE_NET_ENET_DIR}/peer.c
        ${FUSE_NET_ENET_DIR}/protocol.c
    )
    if(WIN32)
        list(APPEND _fuse_enet_sources ${FUSE_NET_ENET_DIR}/win32.c)
    else()
        list(APPEND _fuse_enet_sources ${FUSE_NET_ENET_DIR}/unix.c)
    endif()
    add_library(fuse_net_enet STATIC ${_fuse_enet_sources})
    target_include_directories(fuse_net_enet PUBLIC $<BUILD_INTERFACE:${FUSE_NET_ENET_DIR}/include>)
    set_target_properties(fuse_net_enet PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(WIN32)
        target_compile_definitions(fuse_net_enet PUBLIC WIN32)
        target_link_libraries(fuse_net_enet PUBLIC ws2_32 winmm)
    else()
        target_compile_definitions(fuse_net_enet PRIVATE
            HAS_SOCKLEN_T=1 HAS_POLL=1 HAS_FCNTL=1 HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1)
    endif()
    target_link_libraries(fuse_net PRIVATE fuse_net_enet)
    target_compile_definitions(fuse_net PUBLIC FUSE_NET_HAS_ENET=1)
    set(FUSE_NET_HAS_ENET ON)
    message(STATUS "FUSE: fuse_net ENet backend enabled (${FUSE_NET_ENET_DIR})")
else()
    message(STATUS "FUSE: fuse_net ENet backend disabled — ENetTransport is a stub")
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
