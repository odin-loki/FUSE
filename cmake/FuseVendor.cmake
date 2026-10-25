# Central vendor root for third-party libraries consumed by the FUSE product graph.

set(FUSE_VENDOR_DIR "${CMAKE_SOURCE_DIR}/vendor")
set(FUSE_STB_DIR "${FUSE_VENDOR_DIR}/stb")

function(fuse_vendor_path out_var subpath)
    set(${out_var} "${FUSE_VENDOR_DIR}/${subpath}" PARENT_SCOPE)
endfunction()

function(fuse_stb_include target visibility)
    if(NOT IS_DIRECTORY "${FUSE_STB_DIR}")
        message(FATAL_ERROR "FUSE: STB headers missing at ${FUSE_STB_DIR}")
    endif()
    target_include_directories(${target} ${visibility} "${FUSE_STB_DIR}")
endfunction()

# Single STB image implementation for the product graph (avoid LNK2005 when linking fuse_world2d + fuse_cook_stubs).
function(fuse_ensure_stb_image_target)
    if(TARGET fuse_stb_image)
        return()
    endif()
    if(NOT EXISTS "${FUSE_STB_DIR}/stb_image.h")
        return()
    endif()
    add_library(fuse_stb_image STATIC "${CMAKE_SOURCE_DIR}/cmake/stb_image_impl.cpp")
    add_library(fuse::stb_image ALIAS fuse_stb_image)
    fuse_stb_include(fuse_stb_image PRIVATE)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(fuse_stb_image)
    endif()
    set_target_properties(fuse_stb_image PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()
