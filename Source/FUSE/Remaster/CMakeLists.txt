# FUSE Remaster, Wave 0 foundations (docs/plans/FUSE_REMASTER_PLAN.md §1.3, §2, §7 Wave 0) and the W1.6
# TorqueScript parser. Added once from Source/FUSE/CMakeLists.txt, before Relight (Relight's capture store
# writes through fuse_remaster_store / fuse_remaster_db).
#
# Targets
#   fuse_poco               header-only: POCO structs (plan §2.3), JSON value / reader / writer, SHA-256 / SHA-1 /
#                           UUIDv5 digests (W0.1)
#   fuse_remaster_store     content-addressed blob store + canonical hashing of textures (RGBA8 mip 0), meshes
#                           (canonical vertex / index stream) and sound (PCM) (W0.2)
#   fuse_remaster_db        the replacement DB on vendor/sqlite + the remaster.lock.json export / import (W0.3)
#   fuse_remaster_tscript   TorqueScript parser -> AST, call graph, datablock tree (W1.6)
#   fuse_remaster           the CLI (Tools/FUSE/fuse_remaster.cpp): init, doctor, report (W0.7)
# Gates (ctest -L remaster): remaster_poco_*, remaster_store_*, remaster_db_*, remaster_cli_*, remaster_tscript_*,
# remaster_fuse_ai (pytest-style, Tools/FUSE/Remaster), remaster_no_originals (W0.6).

set(FUSE_REMASTER_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(FUSE_REMASTER_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
get_filename_component(FUSE_REMASTER_REPO_ROOT "${FUSE_REMASTER_REPO_ROOT}" ABSOLUTE)

enable_language(C) # vendor/sqlite

function(_fuse_remaster_cxx target)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${target})
    else()
        set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# ---- W0.1 fuse_poco (header-only) ---------------------------------------------------------------------------
add_library(fuse_poco INTERFACE)
target_include_directories(fuse_poco INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(fuse_poco INTERFACE cxx_std_23)

# ---- W0.2 fuse_remaster_store -------------------------------------------------------------------------------
add_library(fuse_remaster_store STATIC src/store.cpp src/canonical.cpp src/image_decode.cpp)
target_include_directories(fuse_remaster_store PRIVATE "${FUSE_REMASTER_REPO_ROOT}/vendor/stb")
target_link_libraries(fuse_remaster_store PUBLIC fuse_poco)
_fuse_remaster_cxx(fuse_remaster_store)

# ---- W0.3 fuse_remaster_db ----------------------------------------------------------------------------------
add_library(fuse_remaster_sqlite3 STATIC "${FUSE_REMASTER_REPO_ROOT}/vendor/sqlite/sqlite3.c")
set_target_properties(fuse_remaster_sqlite3 PROPERTIES LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)
target_include_directories(fuse_remaster_sqlite3 PUBLIC "${FUSE_REMASTER_REPO_ROOT}/vendor/sqlite")
target_compile_definitions(fuse_remaster_sqlite3 PRIVATE SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0
                                                         SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_DEPRECATED=1)
if(MSVC)
    target_compile_options(fuse_remaster_sqlite3 PRIVATE /w)
else()
    target_compile_options(fuse_remaster_sqlite3 PRIVATE -w)
endif()

add_library(fuse_remaster_db STATIC src/db.cpp)
target_link_libraries(fuse_remaster_db PUBLIC fuse_poco PRIVATE fuse_remaster_sqlite3)
_fuse_remaster_cxx(fuse_remaster_db)

# ---- W1.6 fuse_remaster_tscript -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tscript/CMakeLists.txt")
    add_subdirectory(tscript)
endif()

# ---- W0.7 the CLI -------------------------------------------------------------------------------------------
add_executable(fuse_remaster "${FUSE_REMASTER_REPO_ROOT}/Tools/FUSE/fuse_remaster.cpp")
target_link_libraries(fuse_remaster PRIVATE fuse_remaster_db fuse_remaster_store fuse_poco fuse_remaster_sqlite3 fuse_legacy_dif
                                            ${CMAKE_DL_LIBS})
_fuse_remaster_cxx(fuse_remaster)

# ---- W1.5 fuse_legacy_dif (clean-room Torque interior reader + fixture writer) ------------------------------
add_subdirectory(dif)

# ---- gates --------------------------------------------------------------------------------------------------
if(FUSE_BUILD_CORE_TESTS OR BUILD_TESTING)
    add_subdirectory(tests)
    if(EXISTS "${FUSE_REMASTER_REPO_ROOT}/Tools/FUSE/Remaster/CMakeLists.txt")
        add_subdirectory("${FUSE_REMASTER_REPO_ROOT}/Tools/FUSE/Remaster" "${CMAKE_CURRENT_BINARY_DIR}/tools")
    endif()
endif()
