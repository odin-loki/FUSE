# fuse_app (plan UI5): application shell for UI-only projects + samples.
add_library(fuse_app STATIC src/app.cpp src/services.cpp src/platform_services.cpp src/chrome.cpp)
target_include_directories(fuse_app PUBLIC include PRIVATE ${FUSE_UI_REPO_ROOT}/vendor/stb)
target_link_libraries(fuse_app PUBLIC fuse_ui_doc)
if(WIN32 OR MINGW)
    # no extra libs (CreateMutexA / MoveFileExA are kernel32)
endif()

# fuse_app_native: real OS windows + the Vulkan swapchain presenter. One window backend per platform:
#   Win32 / X11 (libX11 loaded at runtime)   src/native.cpp
#   Android (NativeActivity + app glue)      src/native_android.cpp, src/entry_android.cpp
#   macOS (Cocoa) / iOS (UIKit)              src/native_apple.mm (+ src/entry_ios.mm)
if(ANDROID)
    add_library(fuse_app_native STATIC src/native_android.cpp src/entry_android.cpp)
elseif(APPLE)
    enable_language(OBJCXX)
    add_library(fuse_app_native STATIC src/native_apple.mm)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_sources(fuse_app_native PRIVATE src/entry_ios.mm)
    endif()
    set_source_files_properties(src/native_apple.mm src/entry_ios.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(fuse_app_native PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
else()
    add_library(fuse_app_native STATIC src/native.cpp)
endif()
target_sources(fuse_app_native PRIVATE src/native_common.cpp)
target_link_libraries(fuse_app_native PUBLIC fuse_app)
if(FUSE_UI_VK_LINK)
    target_sources(fuse_app_native PRIVATE src/vk_present.cpp)
    target_link_libraries(fuse_app_native PRIVATE ${FUSE_UI_VK_LINK})
    target_compile_definitions(fuse_app_native PRIVATE FUSE_APP_HAS_VULKAN=1)
    if(TARGET fuse_ui_vk)
        target_link_libraries(fuse_app_native PRIVATE fuse_ui_vk)
        target_compile_definitions(fuse_app_native PRIVATE FUSE_APP_HAS_UI_VK=1)
    endif()
else()
    target_sources(fuse_app_native PRIVATE src/vk_stub.cpp) # CPU raster presenters only
endif()
if(WIN32 OR MINGW)
    target_link_libraries(fuse_app_native PRIVATE user32 gdi32 advapi32 shell32 comdlg32 ole32 uuid)
elseif(ANDROID)
    # android_native_app_glue ships with the NDK as source. NativeActivity looks ANativeActivity_onCreate up by name
    # in the app's shared library: fuse_app_add_executable keeps the symbol (-u).
    set(_glue "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
    if(NOT EXISTS "${_glue}/android_native_app_glue.c")
        set(_glue "${ANDROID_NDK}/sources/android/native_app_glue")
    endif()
    enable_language(C)
    add_library(fuse_app_android_glue STATIC "${_glue}/android_native_app_glue.c")
    target_include_directories(fuse_app_android_glue SYSTEM PUBLIC "${_glue}")
    target_link_libraries(fuse_app_android_glue PUBLIC android log)
    target_link_libraries(fuse_app_native PUBLIC fuse_app_android_glue)
elseif(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(fuse_app_native PUBLIC "-framework UIKit")
    else()
        target_link_libraries(fuse_app_native PUBLIC "-framework Cocoa")
    endif()
    target_link_libraries(fuse_app_native PUBLIC "-framework Foundation" "-framework QuartzCore" "-framework CoreGraphics")
    if(FUSE_UI_VK_LINK)
        target_link_libraries(fuse_app_native PUBLIC "-framework Metal")
    endif()
else()
    target_link_libraries(fuse_app_native PRIVATE ${CMAKE_DL_LIBS})
endif()

# fuse_app_add_executable(<name> <sources...>): an app target on every platform. Desktop: an executable (a .app
# bundle on iOS, and on macOS with FUSE_APP_MACOS_BUNDLE). Android: lib<name>.so for a NativeActivity
# (android.app.lib_name = <name>; see android/). The app's entry is `int FUSE_APP_MAIN(int argc, char** argv)`
# from fuse/app/main.hpp.
# Bundle metadata comes from these variables when set before the call, else from the defaults shown:
#   FUSE_APP_BUNDLE_ID_PREFIX "org.fuse"     the bundle id is <prefix>.<name>
#   FUSE_APP_BUNDLE_VERSION "1"              CFBundleVersion (the build number)
#   FUSE_APP_BUNDLE_SHORT_VERSION "1.0"      CFBundleShortVersionString
# iOS uses apple/Info.plist.ios.in (launch screen, orientations), a macOS bundle apple/Info.plist.macos.in and the
# FUSE icon. Resources go into the bundle with fuse_app_bundle_resources (cmake/fuse_app_packaging.cmake).
option(FUSE_APP_MACOS_BUNDLE "macOS: build fuse_app_add_executable targets as .app bundles" OFF)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/fuse_app_packaging.cmake)
set(FUSE_APP_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")
function(fuse_app_add_executable name)
    if(NOT DEFINED FUSE_APP_BUNDLE_ID_PREFIX)
        set(FUSE_APP_BUNDLE_ID_PREFIX "org.fuse")
    endif()
    if(NOT DEFINED FUSE_APP_BUNDLE_VERSION)
        set(FUSE_APP_BUNDLE_VERSION "1")
    endif()
    if(NOT DEFINED FUSE_APP_BUNDLE_SHORT_VERSION)
        set(FUSE_APP_BUNDLE_SHORT_VERSION "1.0")
    endif()
    if(ANDROID)
        add_library(${name} SHARED ${ARGN})
        target_link_options(${name} PRIVATE "LINKER:-u,ANativeActivity_onCreate")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        add_executable(${name} MACOSX_BUNDLE ${ARGN})
        set_target_properties(${name} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${FUSE_APP_DIR}/apple/Info.plist.ios.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${FUSE_APP_BUNDLE_ID_PREFIX}.${name}"
            MACOSX_BUNDLE_BUNDLE_NAME "${name}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${FUSE_APP_BUNDLE_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${FUSE_APP_BUNDLE_SHORT_VERSION}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${FUSE_APP_BUNDLE_ID_PREFIX}.${name}"
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
    elseif(APPLE AND FUSE_APP_MACOS_BUNDLE)
        add_executable(${name} MACOSX_BUNDLE ${ARGN})
        set_target_properties(${name} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${FUSE_APP_DIR}/apple/Info.plist.macos.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${FUSE_APP_BUNDLE_ID_PREFIX}.${name}"
            MACOSX_BUNDLE_BUNDLE_NAME "${name}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${FUSE_APP_BUNDLE_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${FUSE_APP_BUNDLE_SHORT_VERSION}")
        fuse_app_macos_icon(${name} "${FUSE_UI_REPO_ROOT}/Source/FUSE/Branding/fuse_256.png")
    else()
        add_executable(${name} ${ARGN})
    endif()
    target_link_libraries(${name} PRIVATE fuse_app_native)
endfunction()

# fuse_app_sample_resources(<name>): what the samples ship. Apple bundles get samples/ui (and the fallback fonts)
# in Resources; every platform gets the target <name>_assets, which stages the same files as an APK assets/ tree
# under <build>/assets/<name> (used by the Gradle template in android/).
function(fuse_app_sample_resources name)
    file(GLOB _ui "${FUSE_APP_DIR}/samples/ui/*.fuseui" "${FUSE_APP_DIR}/samples/ui/*.fusestyle")
    file(GLOB_RECURSE _fonts "${FUSE_UI_REPO_ROOT}/vendor/fonts/noto/*.ttf" "${FUSE_UI_REPO_ROOT}/vendor/fonts/twemoji/*.ttf")
    fuse_app_bundle_resources(${name} BASE "${FUSE_APP_DIR}/samples" FILES ${_ui})
    if(_fonts)
        # fonts/<vendor path>, where fuse::text looks for the fallback faces
        fuse_app_bundle_resources(${name} BASE "${FUSE_UI_REPO_ROOT}/vendor" FILES ${_fonts})
    endif()
    fuse_app_stage_assets(${name} OUT "${CMAKE_BINARY_DIR}/assets/${name}" UI_DIR "${FUSE_APP_DIR}/samples/ui" FONTS)
endfunction()

option(FUSE_BUILD_APP_SAMPLES "Build the fuse_app samples (hello_app, notes_app)" ON)
if(FUSE_BUILD_APP_SAMPLES)
    foreach(s hello_app notes_app)
        fuse_app_add_executable(${s} samples/${s}.cpp)
        fuse_app_sample_resources(${s})
        if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
            # packaged resources: APK assets / the bundle's Resources, read through fuse::app::readResource
            target_compile_definitions(${s} PRIVATE FUSE_APP_SAMPLE_UI_DIR="ui")
        else()
            target_compile_definitions(${s} PRIVATE FUSE_APP_SAMPLE_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples/ui")
        endif()
    endforeach()
endif()
