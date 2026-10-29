# FUSE UI — GUI and application framework (docs/plans/FUSE_UI_PLAN.md). UI2 slice: fuse_ui_core, the headless
# retained core (tree, style cascade, layout, events, focus, timers, draw list, CPU reference rasteriser) with an
# immediate-mode front-end and a test driver. Text is a built-in 5x7 font until fuse_text (UI3).

add_library(fuse_ui_core STATIC
    Core/src/context.cpp
    Core/src/cpu_raster.cpp
    Core/src/font5x7.cpp
    Core/src/immediate.cpp
    Core/src/style_sheet.cpp
    Core/src/test_driver.cpp
    Core/src/widgets.cpp
    Core/include/fuse/ui/context.hpp
    Core/include/fuse/ui/cpu_raster.hpp
    Core/include/fuse/ui/draw_list.hpp
    Core/include/fuse/ui/font5x7.hpp
    Core/include/fuse/ui/geometry.hpp
    Core/include/fuse/ui/immediate.hpp
    Core/include/fuse/ui/test_driver.hpp
)
add_library(fuse::ui_core ALIAS fuse_ui_core)
target_include_directories(fuse_ui_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Core/include>)
target_link_libraries(fuse_ui_core PUBLIC fuse_core)
# The CPU rasteriser is the reference for a future GPU backend: no FMA contraction.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    set_source_files_properties(Core/src/cpu_raster.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()

# Sample: a headless FUSE UI app rendered by the CPU rasteriser to a PNG.
add_executable(fuse_ui_hello_app samples/hello_app.cpp)
target_link_libraries(fuse_ui_hello_app PRIVATE fuse_ui_core)
target_include_directories(fuse_ui_hello_app SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/vendor/stb)
