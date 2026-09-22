# B2.11 gate: "zero validation errors". Re-runs the CTest suite with the Khronos validation
# layer forced on (VK_INSTANCE_LAYERS) and fails on any validation message or test failure.
#
# Invoked as a test:  cmake -DFUSE_GATE_BUILD_DIR=<dir> -DFUSE_GATE_SELF=<name> -P <this file>
# Exit code 77 (SKIP_RETURN_CODE) when the validation layer manifest is not installed.

if(NOT FUSE_GATE_BUILD_DIR OR NOT FUSE_GATE_SELF)
  message(FATAL_ERROR "FUSE_GATE_BUILD_DIR and FUSE_GATE_SELF are required")
endif()

set(_manifest_dirs
  "$ENV{VK_ADD_LAYER_PATH}"
  "$ENV{VK_LAYER_PATH}"
  "/usr/share/vulkan/explicit_layer.d"
  "/usr/local/share/vulkan/explicit_layer.d"
  "/etc/vulkan/explicit_layer.d"
  "$ENV{VULKAN_SDK}/share/vulkan/explicit_layer.d"
  "$ENV{VULKAN_SDK}/etc/vulkan/explicit_layer.d")
set(_layer_found FALSE)
foreach(_dir IN LISTS _manifest_dirs)
  if(_dir AND EXISTS "${_dir}/VkLayer_khronos_validation.json")
    set(_layer_found TRUE)
  endif()
endforeach()
if(NOT _layer_found)
  message("SKIP: VK_LAYER_KHRONOS_validation not installed")
  cmake_language(EXIT 77)
endif()

set(ENV{VK_INSTANCE_LAYERS} "VK_LAYER_KHRONOS_validation")
cmake_host_system_information(RESULT _cores QUERY NUMBER_OF_LOGICAL_CORES)
execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${FUSE_GATE_BUILD_DIR}" -V --parallel ${_cores}
          -E "^${FUSE_GATE_SELF}$"
  RESULT_VARIABLE _ctest_result
  OUTPUT_VARIABLE _ctest_out
  ERROR_VARIABLE _ctest_out)

string(REGEX MATCHALL "Validation (Error|Warning|Performance Warning): \\[ [A-Za-z0-9_.-]+ \\]"
       _hits "${_ctest_out}")
list(LENGTH _hits _hit_count)
if(_hit_count GREATER 0)
  list(REMOVE_DUPLICATES _hits)
  string(REPLACE ";" "\n  " _unique "${_hits}")
  message("Vulkan validation messages (${_hit_count} total), unique:\n  ${_unique}")
  message(FATAL_ERROR "B2.11 validation gate failed")
endif()
if(NOT _ctest_result EQUAL 0)
  message("${_ctest_out}")
  message(FATAL_ERROR "suite failed under validation layers (ctest exit ${_ctest_result})")
endif()
message("B2.11 validation gate: zero validation messages")
