# FUSE UI — retained-mode GUI / application framework (docs/plans/FUSE_UI_PLAN.md, docs/ui/README.md).
# Builds inside the FUSE tree (Source/FUSE/CMakeLists.txt) or standalone for fast iteration:
#   cmake -S Source/FUSE/UI -B build-ui -G Ninja && cmake --build build-ui && ctest --test-dir build-ui
# fuse_text and fuse_ui_core depend only on the standard library and vendored stb (not fuse_core), so the
# toolkit can be built and tested without the engine.
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FUSE_UI LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()

get_filename_component(FUSE_UI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

option(FUSE_BUILD_UI_WIDGETS "Build fuse_ui_widgets (Tier A widgets)" ON)
option(FUSE_BUILD_UI_TESTS "Build FUSE UI gate tests" ON)
option(FUSE_BUILD_UI_GAME "Build fuse_ui_game (UI7: HUD, world-space UI, gamepad navigation)" ON)
option(FUSE_BUILD_APP "Build fuse_app (application framework; UI9, not yet)" OFF)
option(FUSE_BUILD_UI_DESIGNER "Build fuse_ui_designer (UI8 prototype: library, gate and app)" ON)
option(FUSE_UI_ACCESSIBILITY "Accessibility tree (AccessKit bridge later, D7)" ON)
set(FUSE_UI_TEXT "MINIMAL" CACHE STRING "Text profile: FULL (HarfBuzz + FreeType, fetched), MINIMAL (stb_truetype), COOKED (not yet)")
set_property(CACHE FUSE_UI_TEXT PROPERTY STRINGS FULL MINIMAL COOKED)
set(FUSE_UI_PROFILE "app" CACHE STRING "UI profile: hud, app, editor")
if(FUSE_UI_TEXT STREQUAL "COOKED")
    message(STATUS "FUSE UI: text profile COOKED not implemented yet — using MINIMAL")
endif()

# ---- fuse_text ----
add_library(fuse_text STATIC Text/src/text.cpp Text/src/font_family.cpp Text/src/i18n.cpp)
target_include_directories(fuse_text PUBLIC Text/include)
target_include_directories(fuse_text SYSTEM PRIVATE "${FUSE_UI_REPO_ROOT}/vendor/stb")
target_compile_definitions(fuse_text PRIVATE
    FUSE_TEXT_BUNDLED_FONT="${FUSE_UI_REPO_ROOT}/vendor/fonts/opensans/OpenSans.ttf"
    FUSE_TEXT_FONT_DIR="${FUSE_UI_REPO_ROOT}/vendor/fonts")
target_compile_features(fuse_text PUBLIC cxx_std_23)
# Android / iOS apps have no build-tree font directory at run time: embed the default UI face in the binary
# (fallback faces come from APK assets / the app bundle through fuse::text::setFontDataSource).
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_fuse_embed_font_default ON)
else()
    set(_fuse_embed_font_default OFF)
endif()
option(FUSE_TEXT_EMBED_FONT "Embed the bundled Open Sans face in fuse_text (default on Android / iOS)" ${_fuse_embed_font_default})
if(FUSE_TEXT_EMBED_FONT)
    set(_font_gen "${CMAKE_CURRENT_BINARY_DIR}/text_gen")
    file(MAKE_DIRECTORY "${_font_gen}")
    add_custom_command(OUTPUT "${_font_gen}/fuse_embedded_font.h"
        COMMAND "${CMAKE_COMMAND}" -DIN=${FUSE_UI_REPO_ROOT}/vendor/fonts/opensans/OpenSans.ttf
                -DOUT=${_font_gen}/fuse_embedded_font.h -DNAME=kFuseEmbeddedFont
                -P "${CMAKE_CURRENT_SOURCE_DIR}/Text/cmake/embed_bin.cmake"
        DEPENDS "${FUSE_UI_REPO_ROOT}/vendor/fonts/opensans/OpenSans.ttf" "${CMAKE_CURRENT_SOURCE_DIR}/Text/cmake/embed_bin.cmake"
        COMMENT "Embedding OpenSans.ttf" VERBATIM)
    target_sources(fuse_text PRIVATE "${_font_gen}/fuse_embedded_font.h")
    target_include_directories(fuse_text PRIVATE "${_font_gen}")
    target_compile_definitions(fuse_text PRIVATE FUSE_TEXT_EMBEDDED_FONT=1)
endif()

# FULL profile (decision D2): HarfBuzz + FreeType through FetchContent (static, no optional deps), SheenBidi (UAX #9)
# and libunibreak (UAX #14) fetched and compiled from their C sources.
if(FUSE_UI_TEXT STREQUAL "FULL")
    include(FetchContent)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(fuse_freetype GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
        GIT_TAG VER-2-13-3 GIT_SHALLOW TRUE EXCLUDE_FROM_ALL)
    FetchContent_Declare(fuse_harfbuzz GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG 10.1.0 GIT_SHALLOW TRUE EXCLUDE_FROM_ALL)
    set(_fuse_bsl ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(fuse_freetype fuse_harfbuzz)
    set(BUILD_SHARED_LIBS ${_fuse_bsl})
    FetchContent_Declare(fuse_sheenbidi GIT_REPOSITORY https://github.com/Tehreer/SheenBidi.git
        GIT_TAG v2.7 GIT_SHALLOW TRUE SOURCE_SUBDIR fuse_no_cmake)
    FetchContent_Declare(fuse_unibreak GIT_REPOSITORY https://github.com/adah1972/libunibreak.git
        GIT_TAG libunibreak_6_1 GIT_SHALLOW TRUE SOURCE_SUBDIR fuse_no_cmake)
    # SOURCE_SUBDIR names a directory that does not exist: download only, the C sources are compiled below.
    FetchContent_MakeAvailable(fuse_sheenbidi fuse_unibreak)
    enable_language(C)
    add_library(fuse_sheenbidi STATIC "${fuse_sheenbidi_SOURCE_DIR}/Source/SheenBidi.c")
    target_compile_definitions(fuse_sheenbidi PRIVATE SB_CONFIG_UNITY)
    target_include_directories(fuse_sheenbidi SYSTEM PUBLIC "${fuse_sheenbidi_SOURCE_DIR}/Headers")
    target_include_directories(fuse_sheenbidi PRIVATE "${fuse_sheenbidi_SOURCE_DIR}/Source")
    set(_ub "${fuse_unibreak_SOURCE_DIR}/src")
    add_library(fuse_unibreak STATIC ${_ub}/linebreak.c ${_ub}/linebreakdata.c ${_ub}/linebreakdef.c
        ${_ub}/unibreakbase.c ${_ub}/unibreakdef.c ${_ub}/eastasianwidthdef.c ${_ub}/emojidef.c)
    target_include_directories(fuse_unibreak SYSTEM PUBLIC "${_ub}")
    set_target_properties(fuse_sheenbidi fuse_unibreak PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_sources(fuse_text PRIVATE Text/src/text_full.cpp)
    target_compile_definitions(fuse_text PRIVATE FUSE_TEXT_FULL=1)
    target_link_libraries(fuse_text PRIVATE harfbuzz freetype fuse_sheenbidi fuse_unibreak)
endif()

# ---- fuse_ui_core ----
add_library(fuse_ui_core STATIC Core/src/style.cpp Core/src/style_sheets.cpp Core/src/draw_list.cpp Core/src/ui.cpp Core/src/immediate.cpp
    Core/src/interaction.cpp Core/src/actions.cpp
    Core/src/path.cpp Core/src/draw_vector.cpp Core/src/painter.cpp Core/src/svg.cpp Core/src/icons.cpp)
target_include_directories(fuse_ui_core PUBLIC Core/include)
target_link_libraries(fuse_ui_core PUBLIC fuse_text)
if(FUSE_UI_ACCESSIBILITY)
    target_compile_definitions(fuse_ui_core PUBLIC FUSE_UI_ACCESSIBILITY=1)
endif()

if(FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_widgets STATIC Widgets/src/widgets.cpp Widgets/src/widgets_tier_b.cpp Widgets/src/widgets_tier_b2.cpp Widgets/src/widgets_tier_cd.cpp Widgets/src/widgets_tier_d2.cpp Widgets/src/widgets_tier_b3.cpp Widgets/src/widgets_svg.cpp Widgets/src/widgets_interaction.cpp Widgets/src/widgets_modelview.cpp Widgets/src/widgets_models.cpp Widgets/src/platform_feel.cpp Widgets/src/theme.cpp Widgets/src/inspector.cpp)
    target_include_directories(fuse_ui_widgets PUBLIC Widgets/include)
    target_link_libraries(fuse_ui_widgets PUBLIC fuse_ui_core)
endif()

# ---- fuse_ui_os + fuse_ui_extras (from the gui-framework branch; docs/ui/archive/PORT_gui_framework.md) ----
# fuse_ui_os is the fuse::ui::os seam (Os/include/fuse/ui/os.hpp) that fuse_ui_extras reaches files, the environment,
# dynamic libraries and helper processes through. Backends:
#   std    (default) Os/src/os_std.cpp: C++ standard library + POSIX / Win32, no third-party code.
#   forge  Os/src/os_forge.cpp on the FORGE library the branch used. FORGE is NOT part of this repository (repository
#          policy: no new git submodules) and nothing is fetched: pass -DFUSE_FORGE_DIR=<your FORGE checkout>.
#   custom your own translation unit(s) in FUSE_UI_OS_SOURCES.
# fuse_ui_extras builds the modules of the branch's Extras/ that compile against main's core API: undo framework,
# declarative properties / state machines, PDF writer, desktop integration, D-Bus client, tray icon + global menu,
# system web view, charts library and 3D data visualisation. The other Extras sources stay in the tree unbuilt
# (gesture, i18n, widgets_extra, button box, rich text, SVG documents, media, the Objective-C++ backends).
# Verified on Linux, Android (NDK) and FreeBSD. Off by default on Windows and Apple platforms: os_std.cpp and the
# Win32 backends compile with MinGW-w64 but were not built with MSVC, and no Apple build was possible.
if(WIN32 OR APPLE)
    set(_fuse_ui_extras_default OFF)
else()
    set(_fuse_ui_extras_default ON)
endif()
option(FUSE_BUILD_UI_EXTRAS "Build fuse_ui_os + fuse_ui_extras (undo, declarative, PDF, desktop, tray, web view, charts, 3D dataviz)" ${_fuse_ui_extras_default})
set(FUSE_UI_OS_BACKEND "std" CACHE STRING "OS layer behind fuse::ui::os: std (standard library + POSIX / Win32), forge (needs FUSE_FORGE_DIR) or custom")
set_property(CACHE FUSE_UI_OS_BACKEND PROPERTY STRINGS std forge custom)
set(FUSE_UI_OS_SOURCES "" CACHE STRING "custom backend: source file(s) implementing fuse/ui/os.hpp")
if(FUSE_BUILD_UI_EXTRAS AND FUSE_BUILD_UI_WIDGETS)
    if(FUSE_UI_OS_BACKEND STREQUAL "forge")
        include("${FUSE_UI_REPO_ROOT}/cmake/FuseForge.cmake")
        fuse_add_forge() # FATAL_ERROR with instructions when FUSE_FORGE_DIR is not a FORGE checkout
        add_library(fuse_ui_os STATIC Os/src/os_forge.cpp)
        target_link_libraries(fuse_ui_os PRIVATE fuse_forge)
    elseif(FUSE_UI_OS_BACKEND STREQUAL "custom")
        if(NOT FUSE_UI_OS_SOURCES)
            message(FATAL_ERROR "FUSE_UI_OS_BACKEND=custom needs FUSE_UI_OS_SOURCES")
        endif()
        add_library(fuse_ui_os STATIC ${FUSE_UI_OS_SOURCES})
    elseif(FUSE_UI_OS_BACKEND STREQUAL "std")
        add_library(fuse_ui_os STATIC Os/src/os_std.cpp)
        target_link_libraries(fuse_ui_os PRIVATE ${CMAKE_DL_LIBS})
    else()
        message(FATAL_ERROR "FUSE_UI_OS_BACKEND must be std, forge or custom (got '${FUSE_UI_OS_BACKEND}')")
    endif()
    if(WIN32 AND NOT FUSE_UI_OS_BACKEND STREQUAL "custom")
        target_sources(fuse_ui_os PRIVATE Os/src/file_dialog_win32.cpp)
        target_link_libraries(fuse_ui_os PRIVATE ole32 shell32 uuid)
    endif()
    target_include_directories(fuse_ui_os PUBLIC Os/include)
    target_compile_features(fuse_ui_os PUBLIC cxx_std_23)

    # (explicit list, not a glob: Extras/src also holds the parked sources)
    add_library(fuse_ui_extras STATIC Extras/src/undo.cpp Extras/src/declarative.cpp Extras/src/pdf.cpp
        Extras/src/desktop.cpp Extras/src/dbus.cpp
        Extras/src/tray.cpp Extras/src/tray_sni.cpp Extras/src/tray_win32.cpp Extras/src/tray_none.cpp
        Extras/src/webview.cpp Extras/src/webview_gtk.cpp Extras/src/webview_win32.cpp Extras/src/webview_none.cpp
        Extras/src/charts.cpp Extras/src/charts_simd.cpp Extras/src/datavis3d.cpp Extras/src/richtext.cpp
        Extras/src/audio_meter.cpp Extras/src/gesture.cpp)
    target_include_directories(fuse_ui_extras PUBLIC Extras/include)
    target_link_libraries(fuse_ui_extras PUBLIC fuse_ui_widgets fuse_ui_os)
    # the branch's code uses partial designated initialisers throughout; in tree that warns under -Wextra
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(fuse_ui_extras PRIVATE -Wno-missing-field-initializers)
    endif()
    if(WIN32)
        target_link_libraries(fuse_ui_extras PUBLIC shell32 user32 gdi32 ole32)
    endif()
    # The branch's AVX2 / NEON min-max kernels for charts (Extras/src/charts_simd_{avx2,neon}.cpp) take their vector
    # type and CPU detection from FORGE's lane layer; they are built only on the forge backend.
    if(TARGET fuse_forge_lane)
        target_link_libraries(fuse_ui_extras PRIVATE fuse_forge fuse_forge_lane)
        target_compile_definitions(fuse_ui_extras PRIVATE FUSE_UI_CHARTS_LANE=1)
        fuse_target_is_x86_64(_ux_x64)
        if(_ux_x64)
            target_sources(fuse_ui_extras PRIVATE Extras/src/charts_simd_avx2.cpp)
            fuse_simd_flags(_ux_avx2 avx2)
            set_source_files_properties(Extras/src/charts_simd_avx2.cpp PROPERTIES COMPILE_OPTIONS "${_ux_avx2}")
        endif()
    endif()
endif()

# ---- fuse_ui_doc: .fuseui JSON documents, binding model and expression bytecode ----
# fuse_ui_sql: SqliteModel, an ItemModel over vendor/sqlite (lazy windowed row cache)
option(FUSE_BUILD_UI_SQL "Build fuse_ui_sql (SQLite item model over vendor/sqlite)" ON)
if(FUSE_BUILD_UI_SQL AND FUSE_BUILD_UI_WIDGETS AND EXISTS "${FUSE_UI_REPO_ROOT}/vendor/sqlite/sqlite3.c")
    enable_language(C)
    add_library(fuse_ui_sqlite3 STATIC "${FUSE_UI_REPO_ROOT}/vendor/sqlite/sqlite3.c")
    set_target_properties(fuse_ui_sqlite3 PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(fuse_ui_sqlite3 PUBLIC "${FUSE_UI_REPO_ROOT}/vendor/sqlite")
    target_compile_definitions(fuse_ui_sqlite3 PRIVATE SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(fuse_ui_sqlite3 PRIVATE -w)
    endif()
    add_library(fuse_ui_sql STATIC Sql/src/sqlite_model.cpp)
    target_include_directories(fuse_ui_sql PUBLIC Sql/include)
    target_link_libraries(fuse_ui_sql PUBLIC fuse_ui_widgets PRIVATE fuse_ui_sqlite3)
    set(FUSE_UI_HAVE_SQL ON)
endif()

# fuse_ui_a11y: accessibility bridge (snapshot / diff / actions); AccessKit backend when FUSE_UI_ACCESSKIT_DIR
# points at an accesskit-c build (include/accesskit.h + lib/libaccesskit.a; needs a Rust toolchain to produce).
if(FUSE_UI_ACCESSIBILITY)
    set(FUSE_UI_ACCESSKIT_DIR "" CACHE PATH "accesskit-c prefix (include/, lib/) for the AccessKit a11y backend; empty = none")
    add_library(fuse_ui_a11y STATIC A11y/src/a11y.cpp A11y/src/a11y_accesskit.cpp)
    target_include_directories(fuse_ui_a11y PUBLIC A11y/include)
    target_link_libraries(fuse_ui_a11y PUBLIC fuse_ui_core)
    find_library(FUSE_UI_ACCESSKIT_LIB NAMES accesskit PATHS "${FUSE_UI_ACCESSKIT_DIR}/lib" NO_DEFAULT_PATH)
    if(FUSE_UI_ACCESSKIT_DIR AND FUSE_UI_ACCESSKIT_LIB AND EXISTS "${FUSE_UI_ACCESSKIT_DIR}/include/accesskit.h")
        target_include_directories(fuse_ui_a11y PRIVATE "${FUSE_UI_ACCESSKIT_DIR}/include")
        target_compile_definitions(fuse_ui_a11y PUBLIC FUSE_UI_HAVE_ACCESSKIT=1)
        find_package(Threads REQUIRED)
        target_link_libraries(fuse_ui_a11y PRIVATE "${FUSE_UI_ACCESSKIT_LIB}" Threads::Threads ${CMAKE_DL_LIBS} m)
        message(STATUS "FUSE UI: AccessKit backend from ${FUSE_UI_ACCESSKIT_DIR}")
    else()
        target_compile_definitions(fuse_ui_a11y PUBLIC FUSE_UI_HAVE_ACCESSKIT=0)
    endif()
endif()

# ---- fuse_ui_l10n_lib: CLDR plural rules, ICU-style messages, locale number / date / time / currency formatting and
# string tables (standard library only). fuse_ui_game's StringTable formats with it; widgets reach it through
# UiContext::setLocale hooks (fuse/ui/l10n/ui_locale.hpp), so binaries that never localise do not link the tables.
add_library(fuse_ui_l10n_lib STATIC L10n/src/plural.cpp L10n/src/format.cpp L10n/src/message.cpp L10n/src/table.cpp)
target_include_directories(fuse_ui_l10n_lib PUBLIC L10n/include)
target_include_directories(fuse_ui_l10n_lib PRIVATE Text/include) # header-only fuse/text/from_chars.hpp; no link to fuse_text
target_compile_features(fuse_ui_l10n_lib PUBLIC cxx_std_23)

option(FUSE_BUILD_UI_DOC "Build fuse_ui_doc (.fuseui documents + bindings)" ON)
if(FUSE_BUILD_UI_DOC AND FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_doc STATIC Doc/src/json.cpp Doc/src/document.cpp Doc/src/cook.cpp Doc/src/hot_reload.cpp
                Doc/src/doc_classes.cpp)
    target_include_directories(fuse_ui_doc PUBLIC Doc/include)
    target_link_libraries(fuse_ui_doc PUBLIC fuse_ui_widgets)
    # UI8 cooker: .fuseui / .fusestyle -> cooked blobs + generated widget-registration list
    add_executable(fuse_ui_cook Doc/tools/fuse_ui_cook.cpp)
    target_link_libraries(fuse_ui_cook PRIVATE fuse_ui_doc)
    # string extraction: tr("...") in C++ and translatable .fuseui text -> merged string-table file
    add_executable(fuse_ui_extract_strings Doc/tools/fuse_ui_extract_strings.cpp)
    target_link_libraries(fuse_ui_extract_strings PRIVATE fuse_ui_doc fuse_ui_l10n_lib)
endif()

# ---- fuse_ui_script: Lua bindings (vendored Lua 5.2, the fuse_script backend) ----
# fuse_ui_video: Theora decode for the video widget (vendored libtheora 1.2.0 + libogg 1.3.6, BSD-3-Clause)
option(FUSE_BUILD_UI_THEORA "Build fuse_ui_video (Theora .ogv decode for the video widget)" ON)
if(FUSE_BUILD_UI_THEORA AND FUSE_BUILD_UI_WIDGETS)
    add_subdirectory(Video)
endif()

# fuse_ui_designer (UI8 prototype): DesignDocument model + Designer UI over fuse_ui_doc
if(FUSE_BUILD_UI_DESIGNER AND FUSE_BUILD_UI_WIDGETS AND FUSE_BUILD_UI_DOC)
    add_library(fuse_ui_designer_lib STATIC Designer/src/designer.cpp)
    target_include_directories(fuse_ui_designer_lib PUBLIC Designer/include)
    target_link_libraries(fuse_ui_designer_lib PUBLIC fuse_ui_doc)
endif()

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
                     HINTS ENV VULKAN_SDK PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /usr/local/lib PATH_SUFFIXES lib Lib)
        find_path(FUSE_UI_VULKAN_INCLUDE vulkan/vulkan.h HINTS ENV VULKAN_SDK PATHS /usr/include /usr/local/include PATH_SUFFIXES include Include)
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
        # In-tree with fuse_rhi on the volk loader (cmake/FuseVolk.cmake puts FUSE_RHI_VOLK=1, VK_NO_PROTOTYPES and
        # the <vulkan/vulkan.h> shim on fuse_rhi's PUBLIC interface) every vk* name in ui_vk.cpp, vk_present.cpp and
        # tests/vk_headless.hpp is a volk function pointer defined in fuse_rhi (they register their raw instances /
        # devices under FUSE_HAS_VULKAN_RHI). Linking the loader's import library as well defines each name twice
        # (MSVC: LNK2005 "vk* already defined in vulkan-1.lib" on fuse_ui_vk_parity), so fuse_rhi is the Vulkan link
        # item then: it brings the headers and the entry points, and the loader is opened at run time.
        if(TARGET fuse_rhi AND FUSE_BUILD_VULKAN)
            get_target_property(_fuse_ui_rhi_defs fuse_rhi INTERFACE_COMPILE_DEFINITIONS)
            if(_fuse_ui_rhi_defs AND "FUSE_RHI_VOLK=1" IN_LIST _fuse_ui_rhi_defs)
                set(_fuse_ui_vk_link fuse_rhi)
            endif()
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
            # The runtime UI stage (UiFrameSource -> frame::FrameUiSource) when the frame composer is in the tree.
            if(TARGET fuse_frame)
                target_link_libraries(fuse_ui_vk PUBLIC fuse_frame)
                target_compile_definitions(fuse_ui_vk PUBLIC FUSE_UI_HAS_FRAME=1)
                # E02 gate: the SceneRenderer UI stage fed by UiFrameSource (Renderer/cmake/scene_renderer.cmake).
                if(TARGET fuse_scene_renderer_vk)
                    target_link_libraries(fuse_scene_renderer_vk PRIVATE fuse_ui_vk)
                    target_compile_definitions(fuse_scene_renderer_vk PRIVATE FUSE_SR_TEST_HAS_FUSE_UI=1)
                    set(_sr_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
                    if(EXISTS "${_sr_lock}" OR FUSE_VULKAN_BACKEND)
                        add_test(NAME fuse_scene_renderer_vk_fuse_ui_t0
                                 COMMAND "${_sr_lock}" "$<TARGET_FILE:fuse_scene_renderer_vk>" --mode fuse_ui --tier t0)
                    else()
                        add_test(NAME fuse_scene_renderer_vk_fuse_ui_t0
                                 COMMAND fuse_scene_renderer_vk --mode fuse_ui --tier t0)
                    endif()
                    # UI7 HUD-less invariant: the UI stage never changes the HUD-less image / TAAU history.
                    if(EXISTS "${_sr_lock}" OR FUSE_VULKAN_BACKEND)
                        add_test(NAME fuse_ui_hudless_history_invariant COMMAND "${_sr_lock}"
                                 "$<TARGET_FILE:fuse_scene_renderer_vk>" --mode hudless_invariant --tier t0)
                    else()
                        add_test(NAME fuse_ui_hudless_history_invariant
                                 COMMAND fuse_scene_renderer_vk --mode hudless_invariant --tier t0)
                    endif()
                    set_tests_properties(fuse_scene_renderer_vk_fuse_ui_t0 fuse_ui_hudless_history_invariant PROPERTIES
                        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" RUN_SERIAL TRUE
                        SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "gate;vulkan;renderer;scene_renderer;ui")
                endif()
            endif()
        endif()
    else()
        message(STATUS "FUSE UI: no Vulkan loader found, fuse_ui_vk skipped (CPU raster only)")
    endif()
endif()

# ---- fuse_app: application shell (UI5) ----
option(FUSE_BUILD_UI_APP "Build fuse_app (Application / AppWindow, on-demand loop) and its samples" ON)
if(FUSE_BUILD_UI_APP AND TARGET fuse_ui_doc)
    add_subdirectory(App)
endif()
if(TARGET fuse_ui_designer_lib AND TARGET fuse_app_native)
    fuse_app_add_executable(fuse_ui_designer Designer/app/designer_main.cpp)
    target_link_libraries(fuse_ui_designer PRIVATE fuse_ui_designer_lib)
endif()

# ---- fuse_ui_game: game UI (UI7) — HUD widgets, world markers / panels, gamepad nav, input arbitration ----
if(FUSE_BUILD_UI_GAME AND FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_game STATIC Game/src/game_ui.cpp Game/src/game_extras.cpp)
    target_include_directories(fuse_ui_game PUBLIC Game/include)
    target_link_libraries(fuse_ui_game PUBLIC fuse_ui_widgets fuse_ui_l10n_lib)
    # engine bridges (in tree only): UiSurface as an ECS component, theme sounds through fuse_audio
    if(TARGET fuse_ecs)
        add_library(fuse_ui_game_ecs STATIC Game/src/ecs_surface.cpp)
        target_link_libraries(fuse_ui_game_ecs PUBLIC fuse_ui_game fuse_ecs)
    endif()
    if(TARGET fuse_audio)
        add_library(fuse_ui_game_audio STATIC Game/src/ui_audio.cpp)
        target_link_libraries(fuse_ui_game_audio PUBLIC fuse_ui_game fuse_audio)
    endif()
    if(TARGET fuse_app)
        foreach(s hud_demo world_panel_demo)
            if(TARGET fuse_app_native)
                fuse_app_add_executable(${s} Game/samples/${s}.cpp)
                target_link_libraries(${s} PRIVATE fuse_ui_game)
            else()
                add_executable(${s} Game/samples/${s}.cpp)
                target_link_libraries(${s} PRIVATE fuse_ui_game fuse_app)
            endif()
        endforeach()
    endif()
endif()

if(FUSE_BUILD_UI_TESTS AND FUSE_BUILD_UI_WIDGETS)
    add_subdirectory(tests)
endif()

# ---- samples that need several modules: widget_gallery (every widget, with the game HUD page when fuse_ui_game is
# built) and game_menu (a .fuseui main / pause / settings menu driven by keyboard, gamepad navigation, mouse, touch) ----
if(TARGET fuse_app_native AND FUSE_BUILD_APP_SAMPLES)
    foreach(s widget_gallery game_menu)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/App/samples/${s}.cpp" OR (s STREQUAL "game_menu" AND NOT TARGET fuse_ui_game))
            continue()
        endif()
        fuse_app_add_executable(${s} App/samples/${s}.cpp)
        fuse_app_sample_resources(${s})
        if(TARGET fuse_ui_game)
            target_link_libraries(${s} PRIVATE fuse_ui_game)
            target_compile_definitions(${s} PRIVATE FUSE_GALLERY_HAVE_GAME=1)
        endif()
        if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
            target_compile_definitions(${s} PRIVATE FUSE_APP_SAMPLE_UI_DIR="ui")
        else()
            target_compile_definitions(${s} PRIVATE FUSE_APP_SAMPLE_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/App/samples/ui")
        endif()
        # headless runs: game_menu plays a scripted session over keyboard, gamepad, mouse and touch and checks every
        # step; widget_gallery shows each page and checks that it is laid out
        if(FUSE_BUILD_UI_TESTS)
            if(s STREQUAL "game_menu")
                add_test(NAME fuse_app_game_menu_sample COMMAND game_menu --screenshot=${CMAKE_CURRENT_BINARY_DIR}/game_menu.png)
                set_tests_properties(fuse_app_game_menu_sample PROPERTIES LABELS "fuse_ui;gate")
            else()
                foreach(p RANGE 0 6)
                    add_test(NAME fuse_app_widget_gallery_page${p}
                             COMMAND widget_gallery --page=${p} --screenshot=${CMAKE_CURRENT_BINARY_DIR}/widget_gallery_${p}.png)
                    set_tests_properties(fuse_app_widget_gallery_page${p} PROPERTIES LABELS "fuse_ui;gate")
                endforeach()
            endif()
        endif()
    endforeach()
endif()

# ---- install rules + the FuseUI CMake package (find_package(FuseUI)); see cmake/fuse_ui_install.cmake ----
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(FUSE_UI_INSTALL "Install the FUSE UI libraries, headers and the FuseUI CMake package" ON)
else()
    option(FUSE_UI_INSTALL "Install the FUSE UI libraries, headers and the FuseUI CMake package" OFF)
endif()
if(FUSE_UI_INSTALL)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fuse_ui_install.cmake)
    # gates: the install tree feeds an out-of-tree consumer; the APK asset staging puts documents and fonts in place
    if(FUSE_BUILD_UI_TESTS AND TARGET fuse_app AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME fuse_ui_install_consumer
                 COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer
                         -DWORK=${CMAKE_CURRENT_BINARY_DIR}/install_consumer -DGENERATOR=${CMAKE_GENERATOR}
                         -DCXX=${CMAKE_CXX_COMPILER} -DCC=${CMAKE_C_COMPILER} -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer/run.cmake)
        set_tests_properties(fuse_ui_install_consumer PROPERTIES LABELS "fuse_ui;gate" TIMEOUT 600)
    endif()
endif()
if(FUSE_BUILD_UI_TESTS AND TARGET fuse_app)
    add_test(NAME fuse_app_stage_assets
             COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/assets_gate -DUI_DIR=${CMAKE_CURRENT_SOURCE_DIR}/App/samples/ui
                     -DFONTS_DIR=${FUSE_UI_REPO_ROOT}/vendor/fonts -DCHECK=1 -P ${CMAKE_CURRENT_SOURCE_DIR}/App/cmake/fuse_app_stage_assets.cmake)
    set_tests_properties(fuse_app_stage_assets PROPERTIES LABELS "fuse_ui;gate")
endif()

# ---- fuse::gui: one umbrella header (<fuse/gui.hpp>, Public/include) and one target for programs in this build ------
# (from the gui-framework branch; outside this build, find_package(FuseUI) and its per-library targets stay the way in)
if(TARGET fuse_app_native AND TARGET fuse_ui_doc)
    add_library(fuse_gui INTERFACE)
    add_library(fuse::gui ALIAS fuse_gui)
    target_include_directories(fuse_gui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Public/include)
    target_link_libraries(fuse_gui INTERFACE fuse_app_native fuse_ui_doc fuse_ui_widgets)
    if(TARGET fuse_ui_extras)
        target_link_libraries(fuse_gui INTERFACE fuse_ui_extras)
    endif()
    if(FUSE_BUILD_APP_SAMPLES AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        add_executable(gui_quickstart Public/examples/gui_quickstart.cpp)
        target_link_libraries(gui_quickstart PRIVATE fuse::gui)
        if(FUSE_BUILD_UI_TESTS AND NOT CMAKE_CROSSCOMPILING)
            add_test(NAME fuse_gui_quickstart COMMAND gui_quickstart --screenshot=${CMAKE_CURRENT_BINARY_DIR}/gui_quickstart.png)
            set_tests_properties(fuse_gui_quickstart PROPERTIES LABELS "fuse_ui;gate")
        endif()
    endif()
endif()
