# Script mode: cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed_bin.cmake
# Writes the bytes of IN as `static const unsigned char NAME[]` (the embedded default UI font on Android / iOS).
file(READ "${IN}" _hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
file(WRITE "${OUT}" "// Generated from ${IN}. Do not edit.\n#pragma once\nstatic const unsigned char ${NAME}[] = {${_bytes}};\n")
