# FUSE installer / package (master plan Appendix A rename checklist: "Icons, installer, docs, CI
# badge names"). Included once, at the end of the top-level CMakeLists (after Tools/FUSE).
#
# * install() rules, all in the single component "fuse" so vendored Engine/lib subprojects that
#   carry their own install() rules (zlib, SDL, assimp, ...) never leak into the FUSE package:
#     bin/                               fuse_editor (when Qt 6 is configured), fuse_cook,
#                                        fuse_convert, fuse_import
#     share/applications/fuse.desktop    Linux desktop entry (Icon=fuse, Exec=fuse_editor)
#     share/icons/hicolor/<N>x<N>/apps/fuse.png, .../scalable/apps/fuse.svg, share/pixmaps/fuse.png
#     share/doc/fuse/                    LICENSE.md, THIRD_PARTY licences of vendored deps
#                                        (enet, lua, vma, fonts/opensans)
# * CPack: package name "fuse", vendor / product "FUSE"; TGZ + DEB on Linux, NSIS + ZIP on Windows
#   (fuse.ico as installer / uninstaller icon), TGZ elsewhere.
# * gate (ctest LABELS "gate;lint"): fuse_package_gate runs cpack -G TGZ into the build tree and
#   verifies contents, icon rasters, the desktop entry (desktop-file-validate when installed) and
#   that no package metadata names Torque/T3D (cmake/FusePackageGate.cmake, self-validating).
#   fuse_branding_icons_regen proves the committed icon files match gen_fuse_icons.py.
#
# The fuse_* tools and editor link fuse_* static libraries, so the package needs no RPATH fix-up;
# fuse_editor relies on the system Qt 6 (DEB Depends) — Windows deployment of Qt DLLs
# (windeployqt) is a release-pipeline step outside this file.

if(DEFINED _FUSE_PACKAGING_INCLUDED)
    return()
endif()
set(_FUSE_PACKAGING_INCLUDED TRUE)

include(GNUInstallDirs)

set(FUSE_BRANDING_DIR "${CMAKE_SOURCE_DIR}/Source/FUSE/Branding")
set(FUSE_PACKAGE_COMPONENT fuse)
set(FUSE_ICON_SIZES 16 32 48 256)

# ---- install rules -------------------------------------------------------------------------------
set(FUSE_PACKAGE_PROGRAMS)
foreach(_t fuse_editor fuse_cook fuse_convert fuse_import)
    if(TARGET ${_t})
        list(APPEND FUSE_PACKAGE_PROGRAMS ${_t})
    endif()
endforeach()
if(FUSE_PACKAGE_PROGRAMS)
    install(TARGETS ${FUSE_PACKAGE_PROGRAMS}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${FUSE_PACKAGE_COMPONENT}
        BUNDLE DESTINATION "." COMPONENT ${FUSE_PACKAGE_COMPONENT})
endif()

set(_fuse_data "${CMAKE_INSTALL_DATADIR}")
foreach(_px IN LISTS FUSE_ICON_SIZES)
    install(FILES "${FUSE_BRANDING_DIR}/fuse_${_px}.png"
        DESTINATION "${_fuse_data}/icons/hicolor/${_px}x${_px}/apps" RENAME fuse.png
        COMPONENT ${FUSE_PACKAGE_COMPONENT})
endforeach()
install(FILES "${FUSE_BRANDING_DIR}/fuse.svg" DESTINATION "${_fuse_data}/icons/hicolor/scalable/apps"
    COMPONENT ${FUSE_PACKAGE_COMPONENT})
install(FILES "${FUSE_BRANDING_DIR}/fuse_48.png" DESTINATION "${_fuse_data}/pixmaps" RENAME fuse.png
    COMPONENT ${FUSE_PACKAGE_COMPONENT})
install(FILES "${FUSE_BRANDING_DIR}/fuse.desktop" DESTINATION "${_fuse_data}/applications"
    COMPONENT ${FUSE_PACKAGE_COMPONENT})

set(_fuse_doc "${_fuse_data}/doc/fuse")
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE.md" "${CMAKE_SOURCE_DIR}/README.md" DESTINATION "${_fuse_doc}"
    COMPONENT ${FUSE_PACKAGE_COMPONENT})
# Vendored third-party licences shipped with the binaries (name -> licence file under Engine/lib).
set(FUSE_PACKAGE_THIRD_PARTY
    "enet=enet/LICENSE.txt"
    "lua=lua/LICENSE.txt"
    "vma=vma/LICENSE.txt"
    "opensans=fonts/opensans/LICENSE.txt")
foreach(_entry IN LISTS FUSE_PACKAGE_THIRD_PARTY)
    string(REPLACE "=" ";" _kv "${_entry}")
    list(GET _kv 0 _name)
    list(GET _kv 1 _file)
    install(FILES "${CMAKE_SOURCE_DIR}/Engine/lib/${_file}" DESTINATION "${_fuse_doc}/third_party/${_name}"
        COMPONENT ${FUSE_PACKAGE_COMPONENT})
endforeach()

# ---- CPack ---------------------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "fuse")
set(CPACK_PACKAGE_VENDOR "FUSE")
set(CPACK_PACKAGE_CONTACT "FUSE maintainers <https://github.com/odin-loki/FUSE>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/odin-loki/FUSE")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "FUSE - Fast Unified Simulation Engine (editor and tools)")
set(CPACK_PACKAGE_DESCRIPTION
    "FUSE is a unified 2D/3D simulation and game engine. This package ships the FUSE Qt 6 editor and the fuse_cook / fuse_convert / fuse_import content tools.")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "FUSE")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.md")
set(CPACK_PACKAGE_CHECKSUM SHA256)
if(TARGET fuse_editor)
    set(CPACK_PACKAGE_EXECUTABLES "fuse_editor;FUSE Editor")
endif()
# Only the FUSE component: vendored subprojects' own install() rules stay out of the package.
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};FUSE;${FUSE_PACKAGE_COMPONENT};/")
set(CPACK_OUTPUT_FILE_PREFIX "packages")

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_PACKAGE_FILE_NAME "fuse-${PROJECT_VERSION}-win64")
    set(CPACK_NSIS_PACKAGE_NAME "FUSE")
    set(CPACK_NSIS_DISPLAY_NAME "FUSE ${PROJECT_VERSION}")
    set(CPACK_NSIS_MUI_ICON "${FUSE_BRANDING_DIR}/fuse.ico")
    set(CPACK_NSIS_MUI_UNIICON "${FUSE_BRANDING_DIR}/fuse.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\fuse_editor.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_HELP_LINK "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH ON)
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_FILE_NAME "fuse-${PROJECT_VERSION}-macos")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_PACKAGE_FILE_NAME "fuse-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_NAME "fuse")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "FUSE maintainers")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
    if(TARGET fuse_editor)
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6 | libqt6widgets6t64")
    endif()
endif()
# Keep the archive rooted at the package name (fuse-<ver>-<sys>/bin/...).
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)

include(CPack)

# ---- gates ---------------------------------------------------------------------------------------
if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME fuse_branding_icons_regen
        COMMAND "${Python3_EXECUTABLE}" "${FUSE_BRANDING_DIR}/gen_fuse_icons.py" --check "${FUSE_BRANDING_DIR}")
    set_tests_properties(fuse_branding_icons_regen PROPERTIES LABELS "gate;lint" TIMEOUT 120)
endif()

# Expected package payload (relative to the archive's top-level directory).
set(_fuse_expected)
foreach(_t IN LISTS FUSE_PACKAGE_PROGRAMS)
    if(_t STREQUAL "fuse_editor" AND APPLE)
        list(APPEND _fuse_expected "fuse_editor.app/Contents/MacOS/fuse_editor")
    else()
        list(APPEND _fuse_expected "${CMAKE_INSTALL_BINDIR}/${_t}${CMAKE_EXECUTABLE_SUFFIX}")
    endif()
endforeach()
foreach(_px IN LISTS FUSE_ICON_SIZES)
    list(APPEND _fuse_expected "${_fuse_data}/icons/hicolor/${_px}x${_px}/apps/fuse.png")
endforeach()
list(APPEND _fuse_expected
    "${_fuse_data}/icons/hicolor/scalable/apps/fuse.svg"
    "${_fuse_data}/pixmaps/fuse.png"
    "${_fuse_data}/applications/fuse.desktop"
    "${_fuse_doc}/LICENSE.md"
    "${_fuse_doc}/README.md")
foreach(_entry IN LISTS FUSE_PACKAGE_THIRD_PARTY)
    string(REPLACE "=" ";" _kv "${_entry}")
    list(GET _kv 0 _name)
    list(GET _kv 1 _file)
    get_filename_component(_leaf "${_file}" NAME)
    list(APPEND _fuse_expected "${_fuse_doc}/third_party/${_name}/${_leaf}")
endforeach()
string(REPLACE ";" "|" _fuse_expected_arg "${_fuse_expected}")

# Tools are required for the gate; fuse_editor is only expected when Qt 6 is configured.
set(_fuse_required_programs fuse_cook fuse_convert fuse_import)
string(REPLACE ";" "|" _fuse_required_arg "${_fuse_required_programs}")
string(REPLACE ";" "|" _fuse_programs_arg "${FUSE_PACKAGE_PROGRAMS}")
string(REPLACE ";" "|" _fuse_sizes_arg "${FUSE_ICON_SIZES}")

set(_fuse_missing_tools)
foreach(_t IN LISTS _fuse_required_programs)
    if(NOT TARGET ${_t})
        list(APPEND _fuse_missing_tools ${_t})
    endif()
endforeach()
if(_fuse_missing_tools)
    # Trimmed configurations (e.g. FUSE_BUILD_PROJECT/FUSE_BUILD_TOOLS OFF) have nothing to package.
    add_test(NAME fuse_package_gate
        COMMAND "${CMAKE_COMMAND}" -E echo "FUSE_PACKAGE_SKIP: tools not configured: ${_fuse_missing_tools}")
    set_tests_properties(fuse_package_gate PROPERTIES LABELS "gate;lint"
        SKIP_REGULAR_EXPRESSION "FUSE_PACKAGE_SKIP: ")
    return()
endif()

add_test(NAME fuse_package_gate
    COMMAND "${CMAKE_COMMAND}"
        "-DFUSE_CPACK=${CMAKE_CPACK_COMMAND}"
        "-DFUSE_CPACK_CONFIG=${CMAKE_BINARY_DIR}/CPackConfig.cmake"
        "-DFUSE_CONFIG=$<CONFIG>"
        "-DFUSE_OUT=${CMAKE_BINARY_DIR}/fuse_package_gate"
        "-DFUSE_EXPECTED=${_fuse_expected_arg}"
        "-DFUSE_PROGRAMS=${_fuse_programs_arg}"
        "-DFUSE_REQUIRED_PROGRAMS=${_fuse_required_arg}"
        "-DFUSE_ICON_SIZES=${_fuse_sizes_arg}"
        "-DFUSE_DATADIR=${_fuse_data}"
        -P "${CMAKE_SOURCE_DIR}/cmake/FusePackageGate.cmake")
set_tests_properties(fuse_package_gate PROPERTIES LABELS "gate;lint" TIMEOUT 600 RUN_SERIAL TRUE)
