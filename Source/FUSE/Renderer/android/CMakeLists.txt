# fuse_runtime_android: fuse_rhi presenting on an Android NativeActivity window (VK_KHR_android_surface), packed
# with the fuse_app APK tooling (UI/App/cmake/FuseAndroidApp.cmake). Gate: fuse_runtime_present_android.
if(NOT DEFINED FUSE_UI_REPO_ROOT)
    set(FUSE_UI_REPO_ROOT "${CMAKE_SOURCE_DIR}")
endif()
fuse_app_add_android_app(fuse_runtime_android SOURCES present_android.cpp PACKAGE org.fuse.runtime LABEL "FUSE Runtime"
                         LIBS fuse_rhi ASSETS "shaders=${CMAKE_CURRENT_SOURCE_DIR}/../shaders/fixtures")
target_include_directories(fuse_runtime_android PRIVATE "${ANDROID_NDK}/sources/android/native_app_glue")
fuse_app_add_android_gate(fuse_runtime_present_android fuse_runtime_android org.fuse.runtime runtime)
