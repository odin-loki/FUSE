# FUSE Relight RL-6.3: game-setup assistant and per-game profiles (docs/plans/FUSE_REMIX_PORT_PLAN.md, RL-6.3).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(setup)
#
# Targets
#   fuse_relight_setup_profile  the profile format (profile.hpp: JSON <-> profile <-> rtx.conf), discovery by exe
#                               name / XXH3 hash and loading as the app-config option layer
#                               (profile_discovery.hpp). Needs only the options and hash libraries, so the tap
#                               links it: d3d9.dll / d3d8.dll load the matching profile when the option system
#                               initialises (tap/src/tap_config.cpp, FUSE_RELIGHT_HAVE_SETUP_PROFILE).
#   fuse_relight_setup          the setup assistant (setup_assistant.hpp): capture record -> proposals -> profile.
#   fuse_relight_setup_tool     CLI fuse_relight_setup (analyze / accept / export-conf / import-conf / match /
#                               hash-exe / check).
# Profiles we ship: Content/relight/profiles/*.json (checked by rl_setup_content).
# Tests: tests/ (rl_setup_unit; rl_setup_content; Wine: rl_setup_wine_<app>).

foreach(_dep fuse_relight_options fuse_relight_hash fuse_relight_tap)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "setup needs ${_dep} (add_subdirectory(setup) after RL-0.5 / RL-0.6 / RL-1.1)")
    endif()
endforeach()

add_library(fuse_relight_setup_profile STATIC
    src/profile.cpp
    src/profile_discovery.cpp
    src/setup_json.cpp
)
target_include_directories(fuse_relight_setup_profile
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fuse_relight_setup_profile PUBLIC fuse_relight_options PRIVATE fuse_relight_hash)

add_library(fuse_relight_setup STATIC src/setup_assistant.cpp)
target_include_directories(fuse_relight_setup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fuse_relight_setup PUBLIC fuse_relight_setup_profile)

add_executable(fuse_relight_setup_tool tools/fuse_relight_setup.cpp)
set_target_properties(fuse_relight_setup_tool PROPERTIES OUTPUT_NAME fuse_relight_setup)
target_link_libraries(fuse_relight_setup_tool PRIVATE fuse_relight_setup)

foreach(_t fuse_relight_setup_profile fuse_relight_setup fuse_relight_setup_tool)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()

# Runtime hook: the tap resolves its options through runtimeOptionSystemDesc() (profile_runtime.hpp), so the
# discovered profile becomes the app-config layer before any option is read. The tap is defined earlier
# (RL-1.1); its one guarded call site is in tap/src/tap_config.cpp.
target_link_libraries(fuse_relight_tap PRIVATE fuse_relight_setup_profile)
target_compile_definitions(fuse_relight_tap PRIVATE FUSE_RELIGHT_HAVE_SETUP_PROFILE=1)

add_subdirectory(tests)
