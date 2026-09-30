# E06 (AP-RT-COOKED, UNI-U7-ASSET-1 render half; docs/plans/FUSE_ASSET_PLAN.md §5.1, docs/unification/TRACK-B-VULKAN.md):
# the renderer loads cooked assets. Owned by the E06 package.
#
#   fuse_rhi (CMakeLists.txt)   src/cooked_assets/bcn_decode.cpp, cooked_texture_gpu.cpp: BC1/4/5/6H/7 CPU decode and the
#                               cooked .fusetex -> VK_FORMAT_BC* (or CPU-decoded) image upload; ResourceManager and
#                               MaterialLayers use them
#   fuse_cooked_assets  STATIC  src/cooked_assets/cooked_mesh.cpp (FMSH v2 meshlets + DAG -> MeshletMesh / ClusterDag),
#                               cooked_asset_registry.cpp (asset::IRenderUploadSink: textures -> bindless, .fusemat ->
#                               layered table + GpuScene rows, meshes -> SceneRenderer's MeshRegistry, ECS sync)
#   fuse_cooked_assets_cpu      ctest  CPU gates (every tree): bcn / fallback / mesh / registry / material / hybrid
#   fuse_cooked_assets_vk_*     ctest  Lavapipe gates (validation + sync validation; exit 77 without Vulkan):
#                               decode (GPU BCn sampling == CPU decode), render (cooked BC7 + BC5 + FMSH == RGBA8 / raw
#                               twin, FLIP), e2e (glTF + PNG -> fuse_cook -> AssetRegistry -> GpuScene + bindless -> image)

get_filename_component(_fuse_ca_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_fuse_ca_inc "${_fuse_ca_root}/include/fuse/renderer/cooked_assets")
set(_fuse_ca_src "${_fuse_ca_root}/src/cooked_assets")

add_library(fuse_cooked_assets STATIC
    ${_fuse_ca_src}/cooked_mesh.cpp
    ${_fuse_ca_src}/cooked_asset_registry.cpp
    ${_fuse_ca_inc}/bcn_decode.hpp
    ${_fuse_ca_inc}/cooked_texture_gpu.hpp
    ${_fuse_ca_inc}/cooked_mesh.hpp
    ${_fuse_ca_inc}/cooked_asset_registry.hpp
)
target_link_libraries(fuse_cooked_assets PUBLIC fuse_scene_renderer fuse_material_layers fuse_geometry_dag fuse_asset fuse_ecs)
fuse_apply_cxx23(fuse_cooked_assets)

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_fuse_ca_tests_dir "${_fuse_ca_root}/tests")

# --- CPU gates -------------------------------------------------------------------------------------------------
# The cook library (Tools/FUSE/Cook, configured after the renderer) encodes the reference blocks and FMSH v2 meshes;
# it is linked when present (generator expressions resolve at generate time).
add_executable(fuse_cooked_assets_cpu ${_fuse_ca_tests_dir}/test_cooked_assets_cpu.cpp)
target_link_libraries(fuse_cooked_assets_cpu PRIVATE fuse_cooked_assets
    $<$<TARGET_EXISTS:fuse_cook_stubs>:fuse_cook_stubs>
    $<$<TARGET_EXISTS:fuse_hybrid>:fuse_hybrid>)
target_compile_definitions(fuse_cooked_assets_cpu PRIVATE
    $<$<TARGET_EXISTS:fuse_cook_stubs>:FUSE_CA_HAS_COOK=1>
    $<$<TARGET_EXISTS:fuse_hybrid>:FUSE_CA_HAS_HYBRID=1>
    FUSE_CA_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/cooked_assets_tmp")
fuse_apply_cxx23(fuse_cooked_assets_cpu)
foreach(_suite bcn fallback mesh registry material hybrid)
    add_test(NAME fuse_cooked_assets_${_suite} COMMAND fuse_cooked_assets_cpu ${_suite})
    set_tests_properties(fuse_cooked_assets_${_suite} PROPERTIES
        LABELS "gate;renderer;cooked_assets;asset" TIMEOUT 600 SKIP_RETURN_CODE 77)
endforeach()

# --- Lavapipe gates --------------------------------------------------------------------------------------------
add_executable(fuse_cooked_assets_vk ${_fuse_ca_tests_dir}/test_cooked_assets_vk.cpp)
target_link_libraries(fuse_cooked_assets_vk PRIVATE fuse_cooked_assets
    $<$<TARGET_EXISTS:fuse_cook_stubs>:fuse_cook_stubs>
    $<$<TARGET_EXISTS:fuse_content_golden_lib>:fuse_content_golden_lib>)
target_compile_definitions(fuse_cooked_assets_vk PRIVATE
    $<$<TARGET_EXISTS:fuse_cook_stubs>:FUSE_CA_HAS_COOK=1>
    $<$<TARGET_EXISTS:fuse_content_golden_lib>:FUSE_CA_HAS_FLIP=1>
    "FUSE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
    FUSE_CA_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/cooked_assets_tmp"
    FUSE_CA_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
fuse_apply_cxx23(fuse_cooked_assets_vk)

# The decode gate's fetch kernel (GLSL, texelFetch of a BCn image into a buffer).
if(FUSE_GLSLANG_VALIDATOR)
    set(_fuse_ca_spv "${CMAKE_CURRENT_BINARY_DIR}/shaders/cooked_assets/bc_fetch.comp.spv")
    get_filename_component(_fuse_ca_spv_dir "${_fuse_ca_spv}" DIRECTORY)
    file(MAKE_DIRECTORY "${_fuse_ca_spv_dir}")
    add_custom_command(OUTPUT "${_fuse_ca_spv}"
        COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 "${_fuse_ca_tests_dir}/cooked_assets/bc_fetch.comp"
                -o "${_fuse_ca_spv}"
        DEPENDS "${_fuse_ca_tests_dir}/cooked_assets/bc_fetch.comp"
        COMMENT "glslangValidator bc_fetch.comp (E06 decode gate)"
        VERBATIM)
    add_custom_target(fuse_cooked_assets_vk_shaders DEPENDS "${_fuse_ca_spv}")
    add_dependencies(fuse_cooked_assets_vk fuse_cooked_assets_vk_shaders)
    target_compile_definitions(fuse_cooked_assets_vk PRIVATE FUSE_CA_FETCH_SPV="${_fuse_ca_spv}")
endif()

# The deferred call runs in the top-level directory scope: hand it this directory's values.
set(FUSE_CA_VK_LOCK "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh" CACHE INTERNAL "E06 ICD lock wrapper")
set(FUSE_CA_VK_BACKEND "${FUSE_VULKAN_BACKEND}" CACHE INTERNAL "E06 Vulkan backend on")
function(_fuse_cooked_assets_register_vk)
    set(_lock "${FUSE_CA_VK_LOCK}")
    set(_tests "")
    foreach(_mode decode render fallback e2e)
        set(_name "fuse_cooked_assets_vk_${_mode}")
        set(_args --mode ${_mode})
        if(_mode STREQUAL "e2e" AND TARGET fuse_cook)
            add_dependencies(fuse_cooked_assets_vk fuse_cook)
            list(APPEND _args --cook "$<TARGET_FILE:fuse_cook>")
        endif()
        if(FUSE_CA_VK_BACKEND)
            add_test(NAME ${_name} COMMAND "${_lock}" "$<TARGET_FILE:fuse_cooked_assets_vk>" ${_args})
        else()
            add_test(NAME ${_name} COMMAND fuse_cooked_assets_vk ${_args})
        endif()
        list(APPEND _tests ${_name})
    endforeach()
    set_tests_properties(${_tests} PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 1800
        LABELS "gate;vulkan;renderer;cooked_assets;asset")
endfunction()
# fuse_cook (Tools/FUSE) is configured after the renderer: register once the whole tree is known.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _fuse_cooked_assets_register_vk)
