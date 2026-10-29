# FUSE UI — retained-mode GUI / application framework (docs/plans/FUSE_UI_PLAN.md, docs/ui/README.md).
# Builds inside the FUSE tree (Source/FUSE/CMakeLists.txt) or standalone for fast iteration:
#   cmake -S Source/FUSE/UI -B build-ui -G Ninja && cmake --build build-ui && ctest --test-dir build-ui
# fuse_text and fuse_ui_core depend only on the standard library and vendored stb (not fuse_core), so the
# toolkit can be built and tested without the engine.
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FUSE_UI LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()

get_filename_component(FUSE_UI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

option(FUSE_BUILD_UI_WIDGETS "Build fuse_ui_widgets (Tier A widgets)" ON)
option(FUSE_BUILD_UI_TESTS "Build FUSE UI gate tests" ON)
option(FUSE_BUILD_UI_GAME "Build fuse_ui_game (HUD/world-space; UI8, not yet)" OFF)
option(FUSE_BUILD_APP "Build fuse_app (application framework; UI9, not yet)" OFF)
option(FUSE_BUILD_UI_DESIGNER "Build the UI designer (UI10, not yet)" OFF)
option(FUSE_UI_ACCESSIBILITY "Accessibility tree (AccessKit bridge later, D7)" ON)
set(FUSE_UI_TEXT "MINIMAL" CACHE STRING "Text profile: FULL (HarfBuzz/ICU, not yet), MINIMAL (stb_truetype), COOKED")
set_property(CACHE FUSE_UI_TEXT PROPERTY STRINGS FULL MINIMAL COOKED)
set(FUSE_UI_PROFILE "app" CACHE STRING "UI profile: hud, app, editor")
if(NOT FUSE_UI_TEXT STREQUAL "MINIMAL")
    message(STATUS "FUSE UI: text profile ${FUSE_UI_TEXT} not implemented yet — using MINIMAL")
endif()

# ---- fuse_text ----
add_library(fuse_text STATIC Text/src/text.cpp)
target_include_directories(fuse_text PUBLIC Text/include)
target_include_directories(fuse_text SYSTEM PRIVATE "${FUSE_UI_REPO_ROOT}/vendor/stb")
target_compile_definitions(fuse_text PRIVATE
    FUSE_TEXT_BUNDLED_FONT="${FUSE_UI_REPO_ROOT}/vendor/fonts/opensans/OpenSans.ttf")
target_compile_features(fuse_text PUBLIC cxx_std_23)

# ---- fuse_ui_core ----
add_library(fuse_ui_core STATIC Core/src/style.cpp Core/src/draw_list.cpp Core/src/ui.cpp Core/src/immediate.cpp)
target_include_directories(fuse_ui_core PUBLIC Core/include)
target_link_libraries(fuse_ui_core PUBLIC fuse_text)
if(FUSE_UI_ACCESSIBILITY)
    target_compile_definitions(fuse_ui_core PUBLIC FUSE_UI_ACCESSIBILITY=1)
endif()

if(FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_widgets STATIC Widgets/src/widgets.cpp Widgets/src/theme.cpp)
    target_include_directories(fuse_ui_widgets PUBLIC Widgets/include)
    target_link_libraries(fuse_ui_widgets PUBLIC fuse_ui_core)
endif()

# ---- fuse_ui_doc: .fuseui JSON documents, binding model and expression bytecode ----
option(FUSE_BUILD_UI_DOC "Build fuse_ui_doc (.fuseui documents + bindings)" ON)
if(FUSE_BUILD_UI_DOC AND FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_doc STATIC Doc/src/json.cpp Doc/src/document.cpp)
    target_include_directories(fuse_ui_doc PUBLIC Doc/include)
    target_link_libraries(fuse_ui_doc PUBLIC fuse_ui_widgets)
endif()

# ---- fuse_ui_script: Lua bindings (vendored Lua 5.2, the fuse_script backend) ----
option(FUSE_BUILD_UI_SCRIPT "Build fuse_ui_script (Lua bindings)" ON)
if(FUSE_BUILD_UI_SCRIPT AND TARGET fuse_ui_doc)
    if(NOT TARGET fuse_lua AND EXISTS "${FUSE_UI_REPO_ROOT}/vendor/lua/CMakeLists.txt")
        add_subdirectory("${FUSE_UI_REPO_ROOT}/vendor/lua" "${CMAKE_CURRENT_BINARY_DIR}/vendor_lua")
    endif()
    if(TARGET fuse_lua)
        add_library(fuse_ui_script STATIC Script/src/lua_ui.cpp)
        target_include_directories(fuse_ui_script PUBLIC Script/include)
        target_link_libraries(fuse_ui_script PUBLIC fuse_ui_doc fuse_lua)
    endif()
endif()

# ---- fuse_ui_render (CPU packing, always) + fuse_ui_vk (GPU backend "ui.draw_list", needs a Vulkan loader) ----
add_library(fuse_ui_render STATIC Render/src/gpu_draw_data.cpp)
target_include_directories(fuse_ui_render PUBLIC Render/include)
target_link_libraries(fuse_ui_render PUBLIC fuse_ui_core)

option(FUSE_BUILD_UI_VK "Build fuse_ui_vk (Vulkan compute backend for the draw list)" ON)
if(FUSE_BUILD_UI_VK)
    if(NOT TARGET Vulkan::Vulkan)
        find_package(Vulkan QUIET)
    endif()
    if(NOT TARGET Vulkan::Vulkan)
        # Cross / conda toolchains often hide the host loader from FindVulkan; look for it directly.
        find_library(FUSE_UI_VULKAN_LOADER NAMES vulkan vulkan-1 libvulkan.so.1
                     HINTS ENV VULKAN_SDK PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib PATH_SUFFIXES lib Lib)
        find_path(FUSE_UI_VULKAN_INCLUDE vulkan/vulkan.h HINTS ENV VULKAN_SDK PATHS /usr/include PATH_SUFFIXES include Include)
        if(FUSE_UI_VULKAN_LOADER AND FUSE_UI_VULKAN_INCLUDE)
            add_library(fuse_ui_vulkan_loader INTERFACE)
            target_link_libraries(fuse_ui_vulkan_loader INTERFACE "${FUSE_UI_VULKAN_LOADER}")
            # Cross / conda toolchains list the host /usr/include as implicit (CMake drops it) without searching it:
            # expose only the Vulkan headers through a private symlink dir.
            set(_vkinc "${CMAKE_CURRENT_BINARY_DIR}/ui_vk_headers")
            file(MAKE_DIRECTORY "${_vkinc}")
            foreach(_d vulkan vk_video)
                if(EXISTS "${FUSE_UI_VULKAN_INCLUDE}/${_d}" AND NOT EXISTS "${_vkinc}/${_d}")
                    file(CREATE_LINK "${FUSE_UI_VULKAN_INCLUDE}/${_d}" "${_vkinc}/${_d}" SYMBOLIC)
                endif()
            endforeach()
            target_include_directories(fuse_ui_vulkan_loader SYSTEM INTERFACE "${_vkinc}")
            set(_fuse_ui_vk_link fuse_ui_vulkan_loader)
        endif()
    else()
        set(_fuse_ui_vk_link Vulkan::Vulkan)
    endif()
    find_program(FUSE_UI_GLSLANG NAMES glslangValidator HINTS ENV VULKAN_SDK PATH_SUFFIXES bin Bin)
    if(_fuse_ui_vk_link)
        set(_gen "${CMAKE_CURRENT_BINARY_DIR}/ui_vk_gen")
        file(MAKE_DIRECTORY "${_gen}")
        set(_src "${CMAKE_CURRENT_SOURCE_DIR}/Render/src/shaders/ui_draw_list.comp")
        set(_fixture "${CMAKE_CURRENT_SOURCE_DIR}/Render/src/shaders/fixtures/ui_draw_list.comp.spv")
        if(FUSE_UI_GLSLANG)
            set(_spv "${_gen}/ui_draw_list.comp.spv")
            add_custom_command(OUTPUT "${_spv}"
                COMMAND "${FUSE_UI_GLSLANG}" --target-env vulkan1.1 "${_src}" -o "${_spv}"
                DEPENDS "${_src}" COMMENT "glslangValidator ui_draw_list.comp" VERBATIM)
        elseif(EXISTS "${_fixture}")
            set(_spv "${_fixture}")
        endif()
        add_library(fuse_ui_vk STATIC Render/src/ui_vk.cpp)
        target_link_libraries(fuse_ui_vk PUBLIC fuse_ui_render PRIVATE ${_fuse_ui_vk_link})
        if(_spv)
            add_custom_command(OUTPUT "${_gen}/ui_draw_list_spv.h"
                COMMAND "${CMAKE_COMMAND}" -DSPV=${_spv} -DOUT=${_gen}/ui_draw_list_spv.h -DNAME=kUiDrawListSpv
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/Render/cmake/embed_spv.cmake"
                DEPENDS "${_spv}" "${CMAKE_CURRENT_SOURCE_DIR}/Render/cmake/embed_spv.cmake" VERBATIM)
            target_sources(fuse_ui_vk PRIVATE "${_gen}/ui_draw_list_spv.h")
            target_include_directories(fuse_ui_vk PRIVATE "${_gen}")
        endif()
        set(FUSE_UI_VK_LINK ${_fuse_ui_vk_link} CACHE INTERNAL "")
        # In-tree with the Vulkan RHI: the render-graph pass wrapper (UiLayerGpu, next to hybrid::SpriteLayerGpu).
        if(TARGET fuse_rhi AND FUSE_BUILD_VULKAN)
            target_sources(fuse_ui_vk PRIVATE Render/src/ui_layer_gpu.cpp)
            target_link_libraries(fuse_ui_vk PUBLIC fuse_rhi)
            target_compile_definitions(fuse_ui_vk PUBLIC FUSE_HAS_VULKAN_RHI=1)
        endif()
    else()
        message(STATUS "FUSE UI: no Vulkan loader found, fuse_ui_vk skipped (CPU raster only)")
    endif()
endif()

if(FUSE_BUILD_UI_TESTS AND FUSE_BUILD_UI_WIDGETS)
    add_subdirectory(tests)
endif()
