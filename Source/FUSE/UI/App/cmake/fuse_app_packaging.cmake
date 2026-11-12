# fuse_app packaging helpers (included by App/CMakeLists.txt).
#
#   fuse_app_bundle_resources(<target> BASE <dir> FILES <file>...)
#       Apple bundles (iOS .app, macOS .app when the target is a MACOSX_BUNDLE): copies each file into the bundle's
#       Resources, keeping its path relative to BASE (BASE/ui/main.fuseui -> Resources/ui/main.fuseui, which is what
#       fuse::app::readResource("ui/main.fuseui") reads). Elsewhere: nothing (desktop apps read the files in place,
#       Android apps ship them as APK assets, see fuse_app_stage_assets).
#
#   fuse_app_stage_assets(<target> OUT <dir> [UI_DIR <dir>] [FONTS])
#       Adds the target <target>_assets that stages an APK's assets/ tree under OUT: UI_DIR -> OUT/ui and, with FONTS,
#       the fallback faces of vendor/fonts (Noto, Twemoji) -> OUT/fonts. Point a Gradle project's assets.srcDirs (or
#       aapt2's -A) at OUT. The staging itself is a CMake script (fuse_app_stage_assets.cmake) and runs on any host.
#
#   fuse_app_macos_icon(<target> <png>)
#       macOS bundles: builds <target>.icns from a PNG with sips + iconutil (both ship with macOS) and sets it as the
#       bundle icon. A no-op when the target is not a bundle or the tools are missing.
#
# The Apple parts are compiled on macOS CI only; they were written on Linux.
set(FUSE_APP_PACKAGING_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

function(fuse_app_bundle_resources target)
    cmake_parse_arguments(ARG "" "BASE" "FILES" ${ARGN})
    if(NOT APPLE OR NOT ARG_FILES)
        return()
    endif()
    get_target_property(_bundle ${target} MACOSX_BUNDLE)
    if(NOT _bundle)
        return()
    endif()
    foreach(_f IN LISTS ARG_FILES)
        get_filename_component(_abs "${_f}" ABSOLUTE)
        set(_sub "")
        if(ARG_BASE)
            get_filename_component(_base "${ARG_BASE}" ABSOLUTE)
            file(RELATIVE_PATH _rel "${_base}" "${_abs}")
            get_filename_component(_sub "${_rel}" DIRECTORY)
        endif()
        target_sources(${target} PRIVATE "${_abs}")
        if(_sub)
            set_source_files_properties("${_abs}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${_sub}" HEADER_FILE_ONLY ON)
        else()
            set_source_files_properties("${_abs}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources" HEADER_FILE_ONLY ON)
        endif()
    endforeach()
endfunction()

function(fuse_app_stage_assets target)
    cmake_parse_arguments(ARG "FONTS" "OUT;UI_DIR" "" ${ARGN})
    if(NOT ARG_OUT)
        message(FATAL_ERROR "fuse_app_stage_assets(${target}): OUT <dir> is required")
    endif()
    set(_args -DOUT=${ARG_OUT})
    if(ARG_UI_DIR)
        list(APPEND _args -DUI_DIR=${ARG_UI_DIR})
    endif()
    if(ARG_FONTS)
        list(APPEND _args -DFONTS_DIR=${FUSE_UI_REPO_ROOT}/vendor/fonts)
    endif()
    add_custom_target(${target}_assets
        COMMAND "${CMAKE_COMMAND}" ${_args} -P "${FUSE_APP_PACKAGING_DIR}/fuse_app_stage_assets.cmake"
        COMMENT "Staging APK assets for ${target} in ${ARG_OUT}" VERBATIM)
endfunction()

function(fuse_app_macos_icon target png)
    if(NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()
    get_target_property(_bundle ${target} MACOSX_BUNDLE)
    find_program(FUSE_APP_SIPS sips)
    find_program(FUSE_APP_ICONUTIL iconutil)
    if(NOT _bundle OR NOT FUSE_APP_SIPS OR NOT FUSE_APP_ICONUTIL OR NOT EXISTS "${png}")
        return()
    endif()
    set(_set "${CMAKE_CURRENT_BINARY_DIR}/${target}.iconset")
    set(_icns "${CMAKE_CURRENT_BINARY_DIR}/${target}.icns")
    add_custom_command(OUTPUT "${_icns}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_set}"
        COMMAND "${FUSE_APP_SIPS}" -z 16 16 "${png}" --out "${_set}/icon_16x16.png"
        COMMAND "${FUSE_APP_SIPS}" -z 32 32 "${png}" --out "${_set}/icon_16x16@2x.png"
        COMMAND "${FUSE_APP_SIPS}" -z 32 32 "${png}" --out "${_set}/icon_32x32.png"
        COMMAND "${FUSE_APP_SIPS}" -z 64 64 "${png}" --out "${_set}/icon_32x32@2x.png"
        COMMAND "${FUSE_APP_SIPS}" -z 128 128 "${png}" --out "${_set}/icon_128x128.png"
        COMMAND "${FUSE_APP_SIPS}" -z 256 256 "${png}" --out "${_set}/icon_128x128@2x.png"
        COMMAND "${FUSE_APP_SIPS}" -z 256 256 "${png}" --out "${_set}/icon_256x256.png"
        COMMAND "${FUSE_APP_ICONUTIL}" -c icns -o "${_icns}" "${_set}"
        DEPENDS "${png}" COMMENT "Building ${target}.icns" VERBATIM)
    target_sources(${target} PRIVATE "${_icns}")
    set_source_files_properties("${_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources" GENERATED ON)
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE "${target}.icns")
endfunction()
