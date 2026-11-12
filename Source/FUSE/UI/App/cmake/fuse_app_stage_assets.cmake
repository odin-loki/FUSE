# Stages an APK's assets/ tree (see fuse_app_stage_assets in fuse_app_packaging.cmake):
#   cmake -DOUT=<dir> [-DUI_DIR=<dir with .fuseui / .fusestyle files>] [-DFONTS_DIR=<repo>/vendor/fonts]
#         [-DCHECK=1] -P fuse_app_stage_assets.cmake
# UI_DIR is copied to OUT/ui; the fallback faces under FONTS_DIR (every .ttf / .otf except the default Open Sans,
# which is embedded in the binary on Android) to OUT/fonts with their vendor sub-paths, which is where
# fuse::text looks for them ("fonts/<vendor path>"). CHECK=1 fails unless OUT/ui holds a document afterwards.
if(NOT OUT)
    message(FATAL_ERROR "fuse_app_stage_assets: -DOUT=<dir> is required")
endif()
file(MAKE_DIRECTORY "${OUT}")
if(UI_DIR)
    file(GLOB_RECURSE _ui RELATIVE "${UI_DIR}" "${UI_DIR}/*.fuseui" "${UI_DIR}/*.fusestyle" "${UI_DIR}/*.fuseuic" "${UI_DIR}/*.fusestylec"
         "${UI_DIR}/*.png")
    foreach(_f IN LISTS _ui)
        get_filename_component(_d "${_f}" DIRECTORY)
        file(COPY "${UI_DIR}/${_f}" DESTINATION "${OUT}/ui/${_d}")
    endforeach()
endif()
if(FONTS_DIR)
    file(GLOB_RECURSE _fonts RELATIVE "${FONTS_DIR}" "${FONTS_DIR}/*.ttf" "${FONTS_DIR}/*.otf" "${FONTS_DIR}/*.txt"
         "${FONTS_DIR}/LICENSE*") # the faces and their licence texts
    foreach(_f IN LISTS _fonts)
        if(_f MATCHES "^opensans/")
            continue()
        endif()
        get_filename_component(_d "${_f}" DIRECTORY)
        file(COPY "${FONTS_DIR}/${_f}" DESTINATION "${OUT}/fonts/${_d}")
    endforeach()
endif()
if(CHECK)
    file(GLOB _staged "${OUT}/ui/*.fuseui")
    if(NOT _staged)
        message(FATAL_ERROR "fuse_app_stage_assets: no .fuseui under ${OUT}/ui")
    endif()
    if(FONTS_DIR)
        file(GLOB_RECURSE _staged_fonts "${OUT}/fonts/*.ttf" "${OUT}/fonts/*.otf")
        if(NOT _staged_fonts)
            message(FATAL_ERROR "fuse_app_stage_assets: no fonts under ${OUT}/fonts")
        endif()
    endif()
    list(LENGTH _staged _n)
    message(STATUS "fuse_app_stage_assets: ${_n} document(s) staged in ${OUT}/ui")
endif()
