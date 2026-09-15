# Legacy Torque3D root configure — preserved for backward compatibility.
# Invoked from the FUSE umbrella when FUSE_UMBRELLA=OFF, or when TORQUE_APP_NAME
# is set without enabling the umbrella (auto-detected in root CMakeLists.txt).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CONFIGURATION_TYPES "Debug;RelWithDebInfo;Release" CACHE STRING "" FORCE)

find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    option(GIT_IN_USE "use Git" ON)
else()
    option(GIT_IN_USE "use Git" OFF)
endif()

set(TORQUE_APP_NAME "" CACHE STRING "the app name")
if("${TORQUE_APP_NAME}" STREQUAL "")
    message(FATAL_ERROR "Please set TORQUE_APP_NAME first")
endif()

include("${CMAKE_SOURCE_DIR}/Tools/CMake/torque_macros.cmake")
include("${CMAKE_SOURCE_DIR}/Tools/CMake/torque_configs.cmake")

add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)

# When included from the FUSE umbrella, project(FUSE) is already active.
if(NOT FUSE_UMBRELLA)
    project(${TORQUE_APP_NAME})
endif()

set(TORQUE_APP_ROOT_DIRECTORY "${CMAKE_SOURCE_DIR}/My Projects/${TORQUE_APP_NAME}")
set(TORQUE_APP_GAME_DIRECTORY "${TORQUE_APP_ROOT_DIRECTORY}/game")

set(TORQUE_LIB_ROOT_DIRECTORY "${CMAKE_SOURCE_DIR}/Engine/lib")
set(TORQUE_LIB_TARG_DIRECTORY "${CMAKE_BINARY_DIR}/Engine/lib")
set(TORQUE_SOURCE_DIRECTROY "${CMAKE_SOURCE_DIR}/Engine/source")

set(CMAKE_INSTALL_PREFIX "${TORQUE_APP_ROOT_DIRECTORY}" CACHE STRING "" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TORQUE_APP_GAME_DIRECTORY}")
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/Tools/CMake/finders")

foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${TORQUE_APP_GAME_DIRECTORY}")
endforeach()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    if(CMAKE_CXX_SIZEOF_DATA_PTR EQUAL 8)
        set(TORQUE_CPU_ARM64 ON)
    elseif(CMAKE_CXX_SIZEOF_DATA_PTR EQUAL 4)
        set(TORQUE_CPU_ARM32 ON)
    endif()
else()
    if(CMAKE_CXX_SIZEOF_DATA_PTR EQUAL 8)
        set(TORQUE_CPU_X64 ON)
    elseif(CMAKE_CXX_SIZEOF_DATA_PTR EQUAL 4)
        set(TORQUE_CPU_X32 ON)
    endif()
endif()

if(NOT TORQUE_SCRIPT_EXTENSION)
    set(TORQUE_SCRIPT_EXTENSION "tscript" CACHE STRING "The default script extension to use for TorqueScript files")
endif()
mark_as_advanced(TORQUE_SCRIPT_EXTENSION)

set(TORQUE_MODULE_USER_PATH "" CACHE PATH "Additional search path for modules aside from the default Tools/CMake/modules.")
mark_as_advanced(TORQUE_MODULE_USER_PATH)

if(NOT TORQUE_TEMPLATE)
    set(TORQUE_TEMPLATE "BaseGame" CACHE STRING "the template to use")
endif()

if(NOT TORQUE_INSTALLED_TEMPLATE)
    installTemplate(${TORQUE_TEMPLATE})
    set(TORQUE_INSTALLED_TEMPLATE TRUE CACHE BOOL "Whether or not the game template is installed. This may be reset to false (or removed) to force a reinstall.")
endif()

configure_file("${CMAKE_SOURCE_DIR}/Tools/CMake/torqueConfig.h.in" "${TORQUE_APP_ROOT_DIRECTORY}/source/torqueConfig.h")

if(APPLE)
    include("${CMAKE_SOURCE_DIR}/Tools/CMake/torqueMacOSconfigs.cmake")
endif()

add_subdirectory(Engine)
