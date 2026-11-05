# ctest fixture helper (fuse_sample_level_demo_3d_empty): cooks a sample's bundled .mis into the gitignored
# worlds/*.fuselevel the editor / scene gates open from the source tree (Samples/unification/.gitignore keeps the
# cooked level out of git, so a fresh checkout has none).
#
#   cmake -DCONVERT=<fuse_convert> -DMIS=<bundled.mis> [-DGOLDEN=<golden.mis>] -DOUTPUT=<out.fuselevel>
#         -P generate_sample_level.cmake
#
# Source selection mirrors project::resolveParityLegacySource / ensureDefault3DWorldReady (what demo_3d_empty and the
# editor do when the level is missing): the golden mission (Templates/BaseGame ExampleLevel.mis: sun, sky, ambient,
# fog, ground plane) when present, else the sample's bundled stub mission.
#
# Cooks to a temporary file next to OUTPUT and renames it into place, so a test reading the level while the fixture
# runs sees either the previous file or the new one, never a half-written one.

foreach(_v CONVERT MIS OUTPUT)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "generate_sample_level: -D${_v}=... is required")
    endif()
endforeach()
if(DEFINED GOLDEN AND EXISTS "${GOLDEN}")
    set(MIS "${GOLDEN}")
endif()
if(NOT EXISTS "${MIS}")
    message(FATAL_ERROR "generate_sample_level: mission not found: ${MIS}")
endif()

get_filename_component(_dir "${OUTPUT}" DIRECTORY)
get_filename_component(_name "${OUTPUT}" NAME_WE)
string(RANDOM LENGTH 8 _tag)
set(_tmp "${_dir}/.${_name}.${_tag}.tmp.fuselevel")

execute_process(COMMAND "${CONVERT}" --mis "${MIS}" --output "${_tmp}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT EXISTS "${_tmp}")
    file(REMOVE "${_tmp}")
    message(FATAL_ERROR "generate_sample_level: fuse_convert failed (${_rc}):\n${_out}${_err}")
endif()
file(RENAME "${_tmp}" "${OUTPUT}")
file(GLOB _stray "${_dir}/.${_name}.${_tag}.tmp.fuselevel*")
if(_stray)
    file(REMOVE ${_stray})
endif()
message(STATUS "generate_sample_level: ${MIS} -> ${OUTPUT}")
