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
    if(MSVC) # what cmake/FuseWarnings.cmake gives every target in the FUSE tree
        add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8> $<$<COMPILE_LANGUAGE:C,CXX>:/bigobj>
                            $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
    endif()
endif()

get_filename_component(FUSE_UI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
# Gates pin the lavapipe ICD where it exists (Linux CI). Elsewhere (native Windows) they use the system Vulkan driver.
if(EXISTS "/usr/share/vulkan/icd.d/lvp_icd.json")
    set(FUSE_UI_LVP_ENV "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
else()
    set(FUSE_UI_LVP_ENV "FUSE_UI_SYSTEM_ICD=1")
endif()

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
add_library(fuse_text STATIC Text/src/text.cpp Text/src/font_catalog.cpp)
if(WIN32)
    target_link_libraries(fuse_text PRIVATE advapi32) # registry font lists
endif()
target_include_directories(fuse_text PUBLIC Text/include)
target_include_directories(fuse_text SYSTEM PRIVATE "${FUSE_UI_REPO_ROOT}/vendor/stb")
if(ANDROID)
    # APK assets: fuse_app_add_android_app packs vendor/fonts as fonts/, extracted to the working directory at start
    target_compile_definitions(fuse_text PRIVATE FUSE_TEXT_BUNDLED_FONT="fonts/opensans/OpenSans.ttf" FUSE_TEXT_FONT_DIR="fonts")
else()
    target_compile_definitions(fuse_text PRIVATE
        FUSE_TEXT_BUNDLED_FONT="${FUSE_UI_REPO_ROOT}/vendor/fonts/opensans/OpenSans.ttf"
        FUSE_TEXT_FONT_DIR="${FUSE_UI_REPO_ROOT}/vendor/fonts")
endif()
target_compile_features(fuse_text PUBLIC cxx_std_23)

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
add_library(fuse_ui_core STATIC Core/src/style.cpp Core/src/draw_list.cpp Core/src/ui.cpp Core/src/immediate.cpp Core/src/icons.cpp Core/src/effects.cpp)
target_include_directories(fuse_ui_core PUBLIC Core/include)
target_link_libraries(fuse_ui_core PUBLIC fuse_text)
if(FUSE_UI_ACCESSIBILITY)
    target_compile_definitions(fuse_ui_core PUBLIC FUSE_UI_ACCESSIBILITY=1)
endif()

if(FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_widgets STATIC Widgets/src/widgets.cpp Widgets/src/widgets_tier_b.cpp Widgets/src/widgets_tier_b2.cpp Widgets/src/widgets_tier_cd.cpp Widgets/src/widgets_tier_d2.cpp Widgets/src/platform_feel.cpp Widgets/src/theme.cpp Widgets/src/inspector.cpp)
    target_include_directories(fuse_ui_widgets PUBLIC Widgets/include)
    target_link_libraries(fuse_ui_widgets PUBLIC fuse_ui_core)
endif()

# ---- icons: SVG -> .fuseicons cooker (vendored nanosvg, tool time only) + the starter pack cooked at build time ----
option(FUSE_BUILD_UI_ICONS "Build fuse_ui_icon_cook and cook the starter icon pack (vendor/icons/lucide)" ON)
if(FUSE_BUILD_UI_ICONS AND EXISTS "${FUSE_UI_REPO_ROOT}/vendor/nanosvg/nanosvg.h")
    add_library(fuse_ui_icon_cook_lib STATIC Icons/src/icon_cook.cpp)
    target_include_directories(fuse_ui_icon_cook_lib PUBLIC Icons/include)
    # the vendor dir, not vendor/nanosvg: its VERSION file would shadow <version> on case-insensitive file systems
    target_include_directories(fuse_ui_icon_cook_lib SYSTEM PRIVATE "${FUSE_UI_REPO_ROOT}/vendor")
    target_link_libraries(fuse_ui_icon_cook_lib PUBLIC fuse_ui_core)
    add_executable(fuse_ui_icon_cook Icons/tools/fuse_ui_icon_cook.cpp)
    target_link_libraries(fuse_ui_icon_cook PRIVATE fuse_ui_icon_cook_lib)
    if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
        set(FUSE_UI_ICON_SVG_DIR "${FUSE_UI_REPO_ROOT}/vendor/icons/lucide")
        set(FUSE_UI_ICON_PACK "${CMAKE_CURRENT_BINARY_DIR}/icons/fuse_ui_icons.fuseicons")
        set(FUSE_UI_ICON_PACK "${FUSE_UI_ICON_PACK}" CACHE INTERNAL "starter icon pack (build output)")
        file(GLOB _fuse_ui_svgs CONFIGURE_DEPENDS "${FUSE_UI_ICON_SVG_DIR}/*.svg")
        add_custom_command(OUTPUT "${FUSE_UI_ICON_PACK}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/icons"
            COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:fuse_ui_icon_cook> "--out=${FUSE_UI_ICON_PACK}" "${FUSE_UI_ICON_SVG_DIR}"
            DEPENDS fuse_ui_icon_cook ${_fuse_ui_svgs}
            COMMENT "fuse_ui_icon_cook: starter icon pack" VERBATIM)
        add_custom_target(fuse_ui_icons ALL DEPENDS "${FUSE_UI_ICON_PACK}")
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

# fuse_ui_a11y: accessibility bridge (snapshot / diff / actions) + AccessKit adapters (AT-SPI on Linux, UI Automation on
# Windows). AccessKit comes from FUSE_UI_ACCESSKIT_DIR (a prebuilt accesskit-c: include/accesskit.h + lib/accesskit.lib
# or lib/libaccesskit.a) or, with FUSE_UI_ACCESSKIT_BUILD, is fetched and built with cargo (needs Rust >= 1.85).
if(FUSE_UI_ACCESSIBILITY)
    set(FUSE_UI_ACCESSKIT_DIR "" CACHE PATH "accesskit-c prefix (include/, lib/) for the AccessKit a11y backend; empty = none")
    option(FUSE_UI_ACCESSKIT_BUILD "Fetch accesskit-c and build it with cargo when FUSE_UI_ACCESSKIT_DIR is empty" OFF)
    set(FUSE_UI_ACCESSKIT_TAG "0.22.3" CACHE STRING "accesskit-c release tag for FUSE_UI_ACCESSKIT_BUILD")
    add_library(fuse_ui_a11y STATIC A11y/src/a11y.cpp A11y/src/a11y_accesskit.cpp)
    target_include_directories(fuse_ui_a11y PUBLIC A11y/include)
    target_link_libraries(fuse_ui_a11y PUBLIC fuse_ui_core)
    set(_fuse_ak_inc "")
    set(_fuse_ak_lib "")
    if(FUSE_UI_ACCESSKIT_DIR)
        find_library(FUSE_UI_ACCESSKIT_LIB NAMES accesskit PATHS "${FUSE_UI_ACCESSKIT_DIR}/lib" NO_DEFAULT_PATH)
        if(FUSE_UI_ACCESSKIT_LIB AND EXISTS "${FUSE_UI_ACCESSKIT_DIR}/include/accesskit.h")
            set(_fuse_ak_inc "${FUSE_UI_ACCESSKIT_DIR}/include")
            set(_fuse_ak_lib "${FUSE_UI_ACCESSKIT_LIB}")
        endif()
    elseif(FUSE_UI_ACCESSKIT_BUILD)
        find_program(FUSE_UI_CARGO cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")
        set(_fuse_cargo_ver "")
        if(FUSE_UI_CARGO)
            execute_process(COMMAND "${FUSE_UI_CARGO}" --version OUTPUT_VARIABLE _fuse_cargo_out ERROR_QUIET)
            string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _fuse_cargo_ver "${_fuse_cargo_out}")
        endif()
        if(FUSE_UI_CARGO AND _fuse_cargo_ver VERSION_LESS "1.85")
            # older cargo cannot read the version 4 lock file (distro packages, e.g. Ubuntu 24.04's 1.75)
            message(STATUS "FUSE UI: cargo ${_fuse_cargo_ver} is older than 1.85 (accesskit-c needs it); AccessKit backend off")
        elseif(FUSE_UI_CARGO)
            include(FetchContent)
            # SOURCE_SUBDIR without a CMakeLists.txt: only download (accesskit-c's own CMake pulls in Corrosion)
            FetchContent_Declare(fuse_accesskit_c GIT_REPOSITORY https://github.com/AccessKit/accesskit-c.git
                GIT_TAG ${FUSE_UI_ACCESSKIT_TAG} GIT_SHALLOW TRUE SOURCE_SUBDIR no-cmake)
            FetchContent_MakeAvailable(fuse_accesskit_c)
            set(_fuse_ak_target "${CMAKE_BINARY_DIR}/accesskit-cargo")
            if(WIN32)
                set(_fuse_ak_lib "${_fuse_ak_target}/release/accesskit.lib")
            else()
                set(_fuse_ak_lib "${_fuse_ak_target}/release/libaccesskit.a")
            endif()
            add_custom_command(OUTPUT "${_fuse_ak_lib}"
                COMMAND "${FUSE_UI_CARGO}" build --release --lib --locked
                        --manifest-path "${fuse_accesskit_c_SOURCE_DIR}/Cargo.toml" --target-dir "${_fuse_ak_target}"
                COMMENT "Building accesskit-c ${FUSE_UI_ACCESSKIT_TAG} with cargo" USES_TERMINAL VERBATIM)
            add_custom_target(fuse_ui_accesskit_cargo DEPENDS "${_fuse_ak_lib}")
            add_dependencies(fuse_ui_a11y fuse_ui_accesskit_cargo)
            set(_fuse_ak_inc "${fuse_accesskit_c_SOURCE_DIR}/include")
        else()
            message(STATUS "FUSE UI: FUSE_UI_ACCESSKIT_BUILD set but cargo not found; AccessKit backend off")
        endif()
    endif()
    if(_fuse_ak_lib)
        target_include_directories(fuse_ui_a11y PRIVATE "${_fuse_ak_inc}")
        target_compile_definitions(fuse_ui_a11y PUBLIC FUSE_UI_HAVE_ACCESSKIT=1)
        if(WIN32)
            # cargo --print native-static-libs (the windows crate binds UIA itself through raw-dylib)
            target_link_libraries(fuse_ui_a11y PRIVATE "${_fuse_ak_lib}" ntdll userenv ws2_32 dbghelp)
        else()
            find_package(Threads REQUIRED)
            target_link_libraries(fuse_ui_a11y PRIVATE "${_fuse_ak_lib}" Threads::Threads ${CMAKE_DL_LIBS} m)
        endif()
        message(STATUS "FUSE UI: AccessKit backend (${_fuse_ak_lib})")
    else()
        target_compile_definitions(fuse_ui_a11y PUBLIC FUSE_UI_HAVE_ACCESSKIT=0)
    endif()
endif()

option(FUSE_BUILD_UI_DOC "Build fuse_ui_doc (.fuseui documents + bindings)" ON)
if(FUSE_BUILD_UI_DOC AND FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_doc STATIC Doc/src/json.cpp Doc/src/document.cpp Doc/src/cook.cpp)
    target_include_directories(fuse_ui_doc PUBLIC Doc/include)
    target_link_libraries(fuse_ui_doc PUBLIC fuse_ui_widgets)
    # UI8 cooker: .fuseui / .fusestyle -> cooked blobs + generated widget-registration list
    add_executable(fuse_ui_cook Doc/tools/fuse_ui_cook.cpp)
    target_link_libraries(fuse_ui_cook PRIVATE fuse_ui_doc)
    if(TARGET fuse_ui_icon_cook_lib) # --icons=<pack>: .svg inputs cooked into an icon pack in the same run
        target_link_libraries(fuse_ui_cook PRIVATE fuse_ui_icon_cook_lib)
        target_compile_definitions(fuse_ui_cook PRIVATE FUSE_UI_COOK_HAVE_ICONS=1)
    endif()
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
    # In tree with the Vulkan RHI, fuse_rhi defines the vk* entry points itself (vendored volk trampolines). Linking
    # the system loader as well gives MSVC duplicate symbols (LNK2005), so take only the headers and let fuse_rhi
    # provide the functions.
    if(_fuse_ui_vk_link AND TARGET fuse_rhi AND FUSE_BUILD_VULKAN)
        add_library(fuse_ui_vulkan_headers INTERFACE)
        if(TARGET Vulkan::Headers)
            target_link_libraries(fuse_ui_vulkan_headers INTERFACE Vulkan::Headers)
        else()
            target_include_directories(fuse_ui_vulkan_headers SYSTEM INTERFACE
                $<TARGET_PROPERTY:${_fuse_ui_vk_link},INTERFACE_INCLUDE_DIRECTORIES>)
        endif()
        target_link_libraries(fuse_ui_vulkan_headers INTERFACE fuse_rhi)
        set(_fuse_ui_vk_link fuse_ui_vulkan_headers)
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
        add_library(fuse_ui_vk STATIC Render/src/ui_vk.cpp Render/src/ui_vk_effects.cpp Render/src/ui_vk_sdf.cpp)
        target_link_libraries(fuse_ui_vk PUBLIC fuse_ui_render PRIVATE ${_fuse_ui_vk_link})
        if(_spv)
            add_custom_command(OUTPUT "${_gen}/ui_draw_list_spv.h"
                COMMAND "${CMAKE_COMMAND}" -DSPV=${_spv} -DOUT=${_gen}/ui_draw_list_spv.h -DNAME=kUiDrawListSpv
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/Render/cmake/embed_spv.cmake"
                DEPENDS "${_spv}" "${CMAKE_CURRENT_SOURCE_DIR}/Render/cmake/embed_spv.cmake" VERBATIM)
            target_sources(fuse_ui_vk PRIVATE "${_gen}/ui_draw_list_spv.h")
            target_include_directories(fuse_ui_vk PRIVATE "${_gen}")
        endif()
        # custom effects (built-ins: desaturate, ripple, grain) and the SDF-quad backend shaders
        include(Render/cmake/fuse_ui_effects.cmake)
        foreach(_fx Desaturate Ripple Grain)
            string(TOLOWER ${_fx} _fxl)
            fuse_ui_add_effect(fuse_ui_vk NAME kUiFx${_fx} SOURCE Render/src/shaders/effects/${_fxl}.glsl)
        endforeach()
        fuse_ui_embed_shader(fuse_ui_vk NAME kUiSdfQuadVert SOURCE Render/src/shaders/ui_sdf_quad.vert)
        fuse_ui_embed_shader(fuse_ui_vk NAME kUiSdfQuadFrag SOURCE Render/src/shaders/ui_sdf_quad.frag)
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
                    # The flock ICD-lock wrapper is a bash script: Linux only; native Windows runs the gate directly.
                    set(_sr_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
                    if(NOT WIN32 AND (EXISTS "${_sr_lock}" OR FUSE_VULKAN_BACKEND))
                        add_test(NAME fuse_scene_renderer_vk_fuse_ui_t0
                                 COMMAND "${_sr_lock}" "$<TARGET_FILE:fuse_scene_renderer_vk>" --mode fuse_ui --tier t0)
                    else()
                        add_test(NAME fuse_scene_renderer_vk_fuse_ui_t0
                                 COMMAND fuse_scene_renderer_vk --mode fuse_ui --tier t0)
                    endif()
                    # UI7 HUD-less invariant: the UI stage never changes the HUD-less image / TAAU history.
                    if(NOT WIN32 AND (EXISTS "${_sr_lock}" OR FUSE_VULKAN_BACKEND))
                        add_test(NAME fuse_ui_hudless_history_invariant COMMAND "${_sr_lock}"
                                 "$<TARGET_FILE:fuse_scene_renderer_vk>" --mode hudless_invariant --tier t0)
                    else()
                        add_test(NAME fuse_ui_hudless_history_invariant
                                 COMMAND fuse_scene_renderer_vk --mode hudless_invariant --tier t0)
                    endif()
                    set_tests_properties(fuse_scene_renderer_vk_fuse_ui_t0 fuse_ui_hudless_history_invariant PROPERTIES
                        ENVIRONMENT "${FUSE_UI_LVP_ENV}" RUN_SERIAL TRUE
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
if(TARGET fuse_ui_designer_lib AND TARGET fuse_app_native AND NOT ANDROID)
    add_executable(fuse_ui_designer Designer/app/designer_main.cpp)
    target_link_libraries(fuse_ui_designer PRIVATE fuse_ui_designer_lib fuse_app_native)
endif()

# ---- fuse_ui_game: game UI (UI7) — HUD widgets, world markers / panels, gamepad nav, input arbitration ----
if(FUSE_BUILD_UI_GAME AND FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_game STATIC Game/src/game_ui.cpp Game/src/game_extras.cpp)
    target_include_directories(fuse_ui_game PUBLIC Game/include)
    target_link_libraries(fuse_ui_game PUBLIC fuse_ui_widgets)
    # engine bridges (in tree only): UiSurface as an ECS component, theme sounds through fuse_audio
    if(TARGET fuse_ecs)
        add_library(fuse_ui_game_ecs STATIC Game/src/ecs_surface.cpp)
        target_link_libraries(fuse_ui_game_ecs PUBLIC fuse_ui_game fuse_ecs)
    endif()
    if(TARGET fuse_audio)
        add_library(fuse_ui_game_audio STATIC Game/src/ui_audio.cpp)
        target_link_libraries(fuse_ui_game_audio PUBLIC fuse_ui_game fuse_audio)
    endif()
    if(TARGET fuse_app AND NOT ANDROID)
        foreach(s hud_demo world_panel_demo)
            add_executable(${s} Game/samples/${s}.cpp)
            target_link_libraries(${s} PRIVATE fuse_ui_game fuse_app)
        endforeach()
    endif()
    if(TARGET fuse_app_native AND FUSE_BUILD_APP_SAMPLES AND NOT ANDROID) # Android apps: fuse_app_add_android_app
        add_executable(game_menu App/samples/game_menu.cpp) # UI5 sample: game main menu + options, gamepad nav
        target_link_libraries(game_menu PRIVATE fuse_ui_game fuse_app_native)
    endif()
endif()

if(FUSE_BUILD_UI_TESTS AND FUSE_BUILD_UI_WIDGETS)
    add_subdirectory(tests)
endif()
