# ctest rl_dxvk_exports (RL-0.2): the vendored-DXVK DLLs are 64-bit PE (PE32+, x86-64) and export
# the D3D9 / D3D8 entry points games import.
#   cmake -DFUSE_OBJDUMP=<objdump> -DFUSE_D3D9=<d3d9.dll> -DFUSE_D3D8=<d3d8.dll> -P check_exports.cmake
set(_d3d9_exports
    Direct3DCreate9 Direct3DCreate9Ex Direct3DCreate9On12 Direct3DCreate9On12Ex
    Direct3DShaderValidatorCreate9 D3DPERF_BeginEvent D3DPERF_EndEvent D3DPERF_GetStatus
    D3DPERF_SetMarker D3DPERF_SetRegion Direct3D9EnableMaximizedWindowedModeShim)
set(_d3d8_exports Direct3DCreate8 ValidatePixelShader ValidateVertexShader DebugSetMute)

if(NOT FUSE_OBJDUMP OR NOT EXISTS "${FUSE_OBJDUMP}")
    message(FATAL_ERROR "rl_dxvk_exports: objdump not found (${FUSE_OBJDUMP})")
endif()

set(_failures 0)
foreach(_which d3d9 d3d8)
    string(TOUPPER "${_which}" _up)
    set(_dll "${FUSE_${_up}}")
    if(NOT EXISTS "${_dll}")
        message(SEND_ERROR "rl_dxvk_exports: ${_dll} missing")
        math(EXPR _failures "${_failures} + 1")
        continue()
    endif()
    execute_process(COMMAND "${FUSE_OBJDUMP}" -p "${_dll}" OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(SEND_ERROR "rl_dxvk_exports: objdump -p ${_dll} failed (${_rc})")
        math(EXPR _failures "${_failures} + 1")
        continue()
    endif()
    if(NOT _out MATCHES "file format pei-x86-64" OR NOT _out MATCHES "Magic[ \t]+020b")
        message(SEND_ERROR "rl_dxvk_exports: ${_dll} is not a PE32+ x86-64 image")
        math(EXPR _failures "${_failures} + 1")
    endif()
    # The export table section starts at "[Ordinal/Name Pointer] Table".
    string(FIND "${_out}" "[Ordinal/Name Pointer] Table" _pos)
    if(_pos LESS 0)
        message(SEND_ERROR "rl_dxvk_exports: ${_dll} has no named export table")
        math(EXPR _failures "${_failures} + 1")
        continue()
    endif()
    string(SUBSTRING "${_out}" ${_pos} -1 _table)
    foreach(_sym IN LISTS _${_which}_exports)
        if(NOT _table MATCHES "\\] ${_sym}\n")
            message(SEND_ERROR "rl_dxvk_exports: ${_dll} does not export ${_sym}")
            math(EXPR _failures "${_failures} + 1")
        endif()
    endforeach()
    # d3d8.dll forwards to D3D9.DLL (ours when it sits next to it).
    if(_which STREQUAL "d3d8" AND NOT _out MATCHES "DLL Name: (d3d9|D3D9)\\.(dll|DLL)")
        message(SEND_ERROR "rl_dxvk_exports: ${_dll} does not import d3d9.dll")
        math(EXPR _failures "${_failures} + 1")
    endif()
    # Self-contained: no MinGW runtime DLLs to stage next to the game.
    if(_out MATCHES "DLL Name: (libstdc\\+\\+-6|libgcc_s_[a-z0-9_]+|libwinpthread-1)\\.dll")
        message(SEND_ERROR "rl_dxvk_exports: ${_dll} imports a MinGW runtime DLL (${CMAKE_MATCH_0})")
        math(EXPR _failures "${_failures} + 1")
    endif()
endforeach()

if(_failures GREATER 0)
    message(FATAL_ERROR "rl_dxvk_exports: ${_failures} failure(s)")
endif()
message(STATUS "rl_dxvk_exports: d3d9.dll and d3d8.dll are PE32+ x86-64 with the expected exports")
