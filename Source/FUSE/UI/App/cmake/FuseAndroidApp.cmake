# Android packaging for fuse_app programs (Gradle-free, no SDK build-tools): the app's main() is built into
# lib<target>.so (renamed fuse_app_main; android_main in fuse_app_native runs it), stripped, and packed with
# App/android/fuse_apk.py into a v2-signed NativeActivity APK (<build>/<target>.apk). Needs Python 3 with the
# `cryptography` package; the debug key is generated in the build directory on first use.
#
#   fuse_app_add_android_app(<target> SOURCES a.cpp ... PACKAGE org.example.app [LABEL "Name"] [ASSETS prefix=dir ...])
#   fuse_app_add_android_gates(<target> <package>)   # host-run ctest gates (adb), skipped (77) without a device

if(NOT ANDROID)
    return()
endif()

set(FUSE_APP_ANDROID_DIR "${CMAKE_CURRENT_LIST_DIR}/../android" CACHE INTERNAL "")
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_EXECUTABLE)
    find_program(Python3_EXECUTABLE NAMES python3 python py NO_CMAKE_FIND_ROOT_PATH)
endif()
find_program(FUSE_ANDROID_ADB adb HINTS "$ENV{ANDROID_HOME}/platform-tools" "$ENV{ANDROID_SDK_ROOT}/platform-tools"
             NO_CMAKE_FIND_ROOT_PATH)
set(FUSE_ANDROID_MIN_SDK 26 CACHE STRING "minSdkVersion of fuse_app APKs")
set(FUSE_ANDROID_TARGET_SDK 35 CACHE STRING "targetSdkVersion of fuse_app APKs")

function(fuse_app_add_android_app target)
    cmake_parse_arguments(A "" "PACKAGE;LABEL" "SOURCES;ASSETS;LIBS" ${ARGN})
    if(NOT A_LABEL)
        set(A_LABEL "${target}")
    endif()
    add_library(${target} SHARED ${A_SOURCES})
    target_link_libraries(${target} PRIVATE fuse_app_native ${A_LIBS})
    target_compile_definitions(${target} PRIVATE main=fuse_app_main)
    if(NOT Python3_EXECUTABLE)
        message(STATUS "FUSE app: no Python 3, ${target}.apk not packaged")
        return()
    endif()
    set(_apk "${CMAKE_BINARY_DIR}/${target}.apk")
    set(_so "${CMAKE_CURRENT_BINARY_DIR}/${target}_apk/lib${target}.so")
    set(_assets "--assets" "fonts=${FUSE_UI_REPO_ROOT}/vendor/fonts")
    foreach(a ${A_ASSETS})
        list(APPEND _assets "--assets" "${a}")
    endforeach()
    add_custom_command(OUTPUT "${_apk}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/${target}_apk"
        COMMAND "${CMAKE_STRIP}" --strip-unneeded -o "${_so}" "$<TARGET_FILE:${target}>"
        COMMAND "${Python3_EXECUTABLE}" "${FUSE_APP_ANDROID_DIR}/fuse_apk.py" --out "${_apk}" --package ${A_PACKAGE}
                --label "${A_LABEL}" --lib-name ${target} --lib "${ANDROID_ABI}=${_so}" ${_assets}
                --min-sdk ${FUSE_ANDROID_MIN_SDK} --target-sdk ${FUSE_ANDROID_TARGET_SDK}
                --key "${CMAKE_BINARY_DIR}/fuse_debug_key.pem"
        DEPENDS ${target} "${FUSE_APP_ANDROID_DIR}/fuse_apk.py"
        COMMENT "Packing ${target}.apk" VERBATIM)
    add_custom_target(${target}_apk ALL DEPENDS "${_apk}")
    set_property(TARGET ${target} PROPERTY FUSE_ANDROID_APK "${_apk}")
endfunction()

function(fuse_app_add_android_gates target package)
    get_property(_apk TARGET ${target} PROPERTY FUSE_ANDROID_APK)
    if(NOT _apk OR NOT Python3_EXECUTABLE)
        return()
    endif()
    set(_adb "${FUSE_ANDROID_ADB}")
    if(NOT _adb)
        set(_adb adb)
    endif()
    foreach(g present lifecycle touch ime process_death)
        if(g STREQUAL "present")
            set(_name fuse_platform_present_android)
        elseif(g STREQUAL "lifecycle" OR g STREQUAL "process_death")
            set(_name fuse_app_${g}_android)
        else()
            set(_name fuse_platform_${g}_android)
        endif()
        add_test(NAME ${_name} COMMAND "${Python3_EXECUTABLE}" "${FUSE_APP_ANDROID_DIR}/android_gate.py" --gate ${g}
                 --apk "${_apk}" --package ${package} --abi ${ANDROID_ABI} --adb "${_adb}")
        set_tests_properties(${_name} PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 600
                             LABELS "gate;android;ui")
    endforeach()
endfunction()
