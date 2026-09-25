# Cross-compile FUSE for 32-bit Windows (x86, i686) with MinGW-w64 (GCC, posix thread model) from Linux.
# Used by FUSE Relight (plan FUSE_REMIX_PORT_PLAN RL-0.3) for the 32-bit side: i686 test apps,
# the bridge client and the rl_env_probe32 check; the rest of FUSE is 64-bit only.
#
#   cmake -S . -B build/mingw -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-i686.cmake \
#     -DFUSE_BUILD_VULKAN=OFF
#
# Tests run through Wine when it is installed (CMAKE_CROSSCOMPILING_EMULATOR), so plain `ctest`
# executes the Windows binaries headlessly. Debian/Ubuntu packages:
#   g++-mingw-w64-i686-posix mingw-w64-tools wine32:i386 (multiarch: dpkg --add-architecture i386)
# The posix thread model is required: FUSE uses std::thread / std::mutex / std::condition_variable.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(FUSE_MINGW_TRIPLE i686-w64-mingw32 CACHE STRING "MinGW-w64 target triple")

find_program(CMAKE_C_COMPILER NAMES ${FUSE_MINGW_TRIPLE}-gcc-posix ${FUSE_MINGW_TRIPLE}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${FUSE_MINGW_TRIPLE}-g++-posix ${FUSE_MINGW_TRIPLE}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${FUSE_MINGW_TRIPLE}-windres windres)
find_program(CMAKE_AR NAMES ${FUSE_MINGW_TRIPLE}-gcc-ar-posix ${FUSE_MINGW_TRIPLE}-ar)
find_program(CMAKE_RANLIB NAMES ${FUSE_MINGW_TRIPLE}-gcc-ranlib-posix ${FUSE_MINGW_TRIPLE}-ranlib)
find_program(CMAKE_NM NAMES ${FUSE_MINGW_TRIPLE}-gcc-nm-posix ${FUSE_MINGW_TRIPLE}-nm)
find_program(CMAKE_OBJDUMP NAMES ${FUSE_MINGW_TRIPLE}-objdump)
find_program(CMAKE_STRIP NAMES ${FUSE_MINGW_TRIPLE}-strip)

set(CMAKE_FIND_ROOT_PATH /usr/${FUSE_MINGW_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# SSE2 scalar float maths, not x87: results (and Relight's float-based hashes) then match the x64
# build bit for bit instead of depending on 80-bit excess precision.
set(CMAKE_C_FLAGS_INIT "-msse2 -mfpmath=sse")
set(CMAKE_CXX_FLAGS_INIT "-msse2 -mfpmath=sse")

# Self-contained executables: no libgcc/libstdc++/libwinpthread DLLs to stage next to each test
# (Wine and a bare Windows box then run them straight from the build tree).
# --large-address-aware: 32-bit games and the bridge client get the full 4 GB under WoW64.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++ -Wl,--large-address-aware")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# ctest runs the Windows test binaries through Wine + Xvfb (fuse-wine-xvfb-run.sh: picks the 32-bit
# loader for PE32 programs and exits 77, a ctest skip, when wine32 or Xvfb is not installed).
# Override with -DCMAKE_CROSSCOMPILING_EMULATOR=... (or set it empty to register tests without
# an emulator, e.g. when the build tree is copied to a real Windows machine).
if(NOT DEFINED CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${CMAKE_CURRENT_LIST_DIR}/fuse-wine-xvfb-run.sh;${CMAKE_BINARY_DIR}/wineprefix-xvfb"
        CACHE STRING "Runs cross-compiled Windows test binaries (ctest)")
endif()
