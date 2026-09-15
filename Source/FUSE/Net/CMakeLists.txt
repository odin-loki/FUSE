# fuse_net — Track B networking scaffolding (B7.4)

set(FUSE_NET_PUBLIC_HEADERS
    include/fuse/net/transport.hpp
    include/fuse/net/serializer.hpp
    include/fuse/net/game_state.hpp
    include/fuse/net/checksum.hpp
    include/fuse/net/input_history.hpp
    include/fuse/net/reconcile.hpp
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

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
