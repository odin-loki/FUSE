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

if(FUSE_BUILD_UI_TESTS AND FUSE_BUILD_UI_WIDGETS)
    add_subdirectory(tests)
endif()
