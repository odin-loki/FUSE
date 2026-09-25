# FUSE platform profile detection and cache options.
# Aligns with docs/unification/architecture-parallel.md and T2D platform matrix.

option(FUSE_PLATFORM_WINDOWS "FUSE target profile: Windows desktop" OFF)
option(FUSE_PLATFORM_LINUX "FUSE target profile: Linux desktop" OFF)
option(FUSE_PLATFORM_MACOS "FUSE target profile: macOS desktop" OFF)
option(FUSE_PLATFORM_IOS "FUSE target profile: iOS" OFF)
option(FUSE_PLATFORM_ANDROID "FUSE target profile: Android" OFF)
option(FUSE_PLATFORM_EMSCRIPTEN "FUSE target profile: Emscripten/web" OFF)

if(EMSCRIPTEN)
    set(FUSE_PLATFORM_EMSCRIPTEN ON CACHE BOOL "FUSE target profile: Emscripten/web" FORCE)
elseif(ANDROID)
    set(FUSE_PLATFORM_ANDROID ON CACHE BOOL "FUSE target profile: Android" FORCE)
elseif(IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(FUSE_PLATFORM_IOS ON CACHE BOOL "FUSE target profile: iOS" FORCE)
elseif(WIN32)
    set(FUSE_PLATFORM_WINDOWS ON CACHE BOOL "FUSE target profile: Windows desktop" FORCE)
elseif(APPLE)
    set(FUSE_PLATFORM_MACOS ON CACHE BOOL "FUSE target profile: macOS desktop" FORCE)
elseif(UNIX)
    set(FUSE_PLATFORM_LINUX ON CACHE BOOL "FUSE target profile: Linux desktop" FORCE)
endif()

set(FUSE_PLATFORM_MOBILE OFF)
if(FUSE_PLATFORM_IOS OR FUSE_PLATFORM_ANDROID)
    set(FUSE_PLATFORM_MOBILE ON)
endif()

set(FUSE_PLATFORM_DESKTOP OFF)
if(FUSE_PLATFORM_WINDOWS OR FUSE_PLATFORM_LINUX OR FUSE_PLATFORM_MACOS)
    set(FUSE_PLATFORM_DESKTOP ON)
endif()

function(fuse_log_platform_profile)
    message(STATUS "FUSE platform profile:")
    message(STATUS "  desktop : ${FUSE_PLATFORM_DESKTOP}")
    message(STATUS "  mobile  : ${FUSE_PLATFORM_MOBILE}")
    message(STATUS "  windows : ${FUSE_PLATFORM_WINDOWS}")
    message(STATUS "  linux   : ${FUSE_PLATFORM_LINUX}")
    message(STATUS "  macos   : ${FUSE_PLATFORM_MACOS}")
    message(STATUS "  ios     : ${FUSE_PLATFORM_IOS}")
    message(STATUS "  android : ${FUSE_PLATFORM_ANDROID}")
    message(STATUS "  emscripten: ${FUSE_PLATFORM_EMSCRIPTEN}")
endfunction()

# MinGW-w64 GCC 13.0–13.2 + static libstdc++ + C++23: <typeinfo> makes type_info::operator==
# constexpr-inline (emitted out of line at -O0), but libstdc++.a(tinfo.o) still carries a strong
# definition of it next to type_info::__equal, which the inline version calls. Any TU that
# compares typeids (std::function, std::regex, shared_ptr deleters) then fails to link with
# "multiple definition of std::type_info::operator==" (GCC PR libstdc++/110572, fixed in 13.3).
# Both definitions are the same library code, so let the linker keep the first one.
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13.3)
    add_link_options(-Wl,--allow-multiple-definition)
    message(STATUS "FUSE: MinGW GCC ${CMAKE_CXX_COMPILER_VERSION} — linking with --allow-multiple-definition (PR 110572 type_info::operator==)")
endif()
