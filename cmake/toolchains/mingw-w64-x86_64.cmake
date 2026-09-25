# Cross-compile FUSE for 64-bit Windows with MinGW-w64 (GCC, posix thread model) from Linux.
#
#   cmake -S . -B build/mingw -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
#     -DFUSE_BUILD_VULKAN=OFF
#
# Tests run through Wine when it is installed (CMAKE_CROSSCOMPILING_EMULATOR), so plain `ctest`
# executes the Windows binaries headlessly. Debian/Ubuntu packages:
#   g++-mingw-w64-x86-64-posix mingw-w64-tools wine64
# The posix thread model is required: FUSE uses std::thread / std::mutex / std::condition_variable.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(FUSE_MINGW_TRIPLE x86_64-w64-mingw32 CACHE STRING "MinGW-w64 target triple")

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

# Self-contained executables: no libgcc/libstdc++/libwinpthread DLLs to stage next to each test
# (Wine and a bare Windows box then run them straight from the build tree).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# ctest runs the Windows test binaries through Wine (headless, prefix inside the build tree).
# Override with -DCMAKE_CROSSCOMPILING_EMULATOR=... (or set it empty to register tests without
# an emulator, e.g. when the build tree is copied to a real Windows machine).
if(NOT DEFINED CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${CMAKE_CURRENT_LIST_DIR}/fuse-wine-run.sh;${CMAKE_BINARY_DIR}/wineprefix"
        CACHE STRING "Runs cross-compiled Windows test binaries (ctest)")
endif()
