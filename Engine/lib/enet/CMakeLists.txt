# ENet 1.3.7 (MIT, see LICENSE.txt) — vendored for FUSE fuse_net's UDP transport.
# Sources are unmodified copies; do not upgrade in place without re-running the net tests.
cmake_minimum_required(VERSION 3.21.0)

if(TARGET fuse_enet)
    return()
endif()

# Always enable C here: language variables are directory-scoped, so a check against the global
# ENABLED_LANGUAGES would skip this when a sibling directory enabled C first.
enable_language(C)

set(_fuse_enet_sources
    callbacks.c
    compress.c
    host.c
    list.c
    packet.c
    peer.c
    protocol.c
)
if(WIN32)
    list(APPEND _fuse_enet_sources win32.c)
else()
    list(APPEND _fuse_enet_sources unix.c)
endif()

add_library(fuse_enet STATIC ${_fuse_enet_sources})
add_library(fuse::enet ALIAS fuse_enet)

target_include_directories(fuse_enet SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(fuse_enet PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_compile_definitions(fuse_enet PUBLIC WIN32)
    target_link_libraries(fuse_enet PUBLIC ws2_32 winmm)
else()
    target_compile_definitions(fuse_enet PRIVATE
        HAS_SOCKLEN_T=1 HAS_POLL=1 HAS_FCNTL=1 HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1)
endif()

# Third-party C: silence warnings for this target only.
if(MSVC)
    target_compile_options(fuse_enet PRIVATE /w)
else()
    target_compile_options(fuse_enet PRIVATE -w)
endif()
