# fuse_audio — B7.2 Spatial Audio Engine (OpenAL output, CPU spatial mix, CUDA reverb facade)

set(FUSE_AUDIO_PUBLIC_HEADERS
    include/fuse/audio/audio_backend.hpp
    include/fuse/audio/audio_clip.hpp
    include/fuse/audio/audio_components.hpp
    include/fuse/audio/audio_desc.hpp
    include/fuse/audio/audio_engine.hpp
    include/fuse/audio/audio_registry.hpp
    include/fuse/audio/attenuation.hpp
    include/fuse/audio/audio_bus.hpp
    include/fuse/audio/conv_reverb_cpu.hpp
    include/fuse/audio/math.hpp
    include/fuse/audio/occlusion.hpp
    include/fuse/audio/reverb_cuda.hpp
    include/fuse/audio/reverb_zones.hpp
    include/fuse/audio/spatial_mixer.hpp
)

set(FUSE_AUDIO_SOURCES
    src/attenuation.cpp
    src/audio_bus.cpp
    src/audio_backend.cpp
    src/audio_clip.cpp
    src/audio_engine.cpp
    src/audio_registry.cpp
    src/conv_reverb_cpu.cpp
    src/occlusion.cpp
    src/reverb_cuda.cpp
    src/reverb_zones.cpp
    src/spatial_mixer.cpp
)

add_library(fuse_audio STATIC
    ${FUSE_AUDIO_SOURCES}
    ${FUSE_AUDIO_PUBLIC_HEADERS}
)

add_library(fuse::audio ALIAS fuse_audio)

target_include_directories(fuse_audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_audio PUBLIC fuse_core)
target_compile_features(fuse_audio PUBLIC cxx_std_17)

option(FUSE_AUDIO_ENABLE_OPENAL "Link fuse_audio against OpenAL when available" ON)
if(FUSE_AUDIO_ENABLE_OPENAL)
    find_package(OpenAL QUIET)
    if(OpenAL_FOUND)
        target_compile_definitions(fuse_audio PUBLIC FUSE_AUDIO_OPENAL=1)
        target_link_libraries(fuse_audio PUBLIC OpenAL::OpenAL)
        message(STATUS "FUSE: fuse_audio using system OpenAL")
    else()
        set(_FUSE_OPENAL_SOFT_DIR "${CMAKE_SOURCE_DIR}/Engine/lib/openal-soft")
        if(EXISTS "${_FUSE_OPENAL_SOFT_DIR}/include/AL/al.h")
            set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
            set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
            set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
            set(LIBTYPE "STATIC" CACHE STRING "" FORCE)
            set(ALSOFT_DEDICATED OFF CACHE BOOL "" FORCE)
            add_subdirectory("${_FUSE_OPENAL_SOFT_DIR}" "${CMAKE_BINARY_DIR}/openal-soft" EXCLUDE_FROM_ALL)
            target_compile_definitions(fuse_audio PUBLIC FUSE_AUDIO_OPENAL=1)
            target_link_libraries(fuse_audio PUBLIC OpenAL)
            message(STATUS "FUSE: fuse_audio using vendored openal-soft")
        else()
            message(STATUS "FUSE: OpenAL not found — fuse_audio uses null backend")
        endif()
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
