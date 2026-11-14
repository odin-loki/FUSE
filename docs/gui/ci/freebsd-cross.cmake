# FreeBSD 14.4 amd64 cross toolchain: extract usr/include usr/lib lib from download.freebsd.org/releases/amd64/14.4-RELEASE/base.txz
# into $FREEBSD_SYSROOT, then cmake -S Source/FUSE/UI -B build-fbsd -G Ninja -DCMAKE_TOOLCHAIN_FILE=docs/gui/ci/freebsd-cross.cmake (clang 18 + lld).
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSROOT $ENV{FREEBSD_SYSROOT})
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-unknown-freebsd14.4)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-freebsd14.4)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
