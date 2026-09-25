# Central vendor root for third-party libraries consumed by the FUSE product graph.
# Torque3D heritage trees live under heritage/torque3d/ and are not part of this graph.

set(FUSE_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(FUSE_VENDOR_DIR "${FUSE_THIRD_PARTY_DIR}/vendor")
set(FUSE_STB_DIR "${FUSE_THIRD_PARTY_DIR}/stb")

function(fuse_vendor_path out_var subpath)
    set(${out_var} "${FUSE_VENDOR_DIR}/${subpath}" PARENT_SCOPE)
endfunction()

function(fuse_stb_include target visibility)
    if(NOT IS_DIRECTORY "${FUSE_STB_DIR}")
        message(FATAL_ERROR "FUSE: STB headers missing at ${FUSE_STB_DIR}")
    endif()
    target_include_directories(${target} ${visibility} "${FUSE_STB_DIR}")
endfunction()
