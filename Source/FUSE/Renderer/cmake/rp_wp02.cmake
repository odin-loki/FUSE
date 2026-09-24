# WP-0.2 (docs/unification/RENDERER-EXECUTION.md): load Vulkan through the vendored volk meta-loader.
# Owned by WP-0.2; other packages must not edit this file. cmake/FuseVolk.cmake holds the wiring
# (VK_NO_PROTOTYPES, the <vulkan/vulkan.h> shim, dropping the link-time loader).

include(${CMAKE_SOURCE_DIR}/cmake/FuseVolk.cmake)

# Static loader auto-init (vk/loader.hpp "Loader initialisation"). ON keeps the WP-0.2 behaviour:
# every image linking fuse_rhi opens the loader during static init unless it sets the target
# property FUSE_RHI_VOLK_NO_AUTO_INIT. OFF: no image does; the loader opens on the first explicit
# vkloader::initialize() / VulkanInstance::create / VulkanDevice::adopt.
option(FUSE_RHI_VOLK_AUTO_INIT "fuse_rhi opens the Vulkan loader (volk) during static initialisation" ON)

target_sources(fuse_rhi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/vk/loader.hpp)
if(FUSE_VULKAN_BACKEND)
    fuse_volk_attach(fuse_rhi IMPLEMENTATION ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/vk_loader.cpp)
    set(_fuse_rp_wp02_auto_off "")
    if(NOT FUSE_RHI_VOLK_AUTO_INIT)
        set(_fuse_rp_wp02_auto_off DISABLED)
    endif()
    fuse_volk_auto_init(fuse_rhi SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/vk_loader_autoinit.cpp
                        SYMBOL fuse_rhi_volk_auto_init ${_fuse_rp_wp02_auto_off})
else()
    # Stub build: the loader API compiles to no-ops.
    target_sources(fuse_rhi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/vk_loader.cpp
                                    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/vk_loader_autoinit.cpp)
endif()

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_rp_wp02_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")

    # Gate: volk global/instance/device loading, device-level dispatch with one live device,
    # loader trampolines with two, reload hook, real work on each device, zero validation messages
    # (validation + sync validation). Exit 77 = skip (stub build, no ICD).
    add_executable(fuse_rp_volk_loader tests/test_rp_volk_loader.cpp)
    target_link_libraries(fuse_rp_volk_loader PRIVATE fuse_rhi)
    if(FUSE_VULKAN_BACKEND)
        add_test(NAME fuse_rp_volk_loader COMMAND "${_fuse_rp_wp02_lock}" "$<TARGET_FILE:fuse_rp_volk_loader>")
    else()
        add_test(NAME fuse_rp_volk_loader COMMAND fuse_rp_volk_loader)
    endif()
    set_tests_properties(fuse_rp_volk_loader PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        LABELS "gate;vulkan")

    # Gate: nm/readelf over fuse_rhi and a test executable: no vk* import except vkGetInstanceProcAddr,
    # no DT_NEEDED libvulkan (ELF only; other platforms and the stub build skip).
    if(FUSE_VULKAN_BACKEND AND CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF" AND CMAKE_NM AND CMAKE_READELF)
        add_test(NAME fuse_rp_volk_no_loader_imports
                 COMMAND "${CMAKE_COMMAND}" -DNM=${CMAKE_NM} -DREADELF=${CMAKE_READELF}
                         -DEXE=$<TARGET_FILE:fuse_rp_volk_loader> -DARCHIVE=$<TARGET_FILE:fuse_rhi>
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp02_imports.cmake")
    else()
        add_test(NAME fuse_rp_volk_no_loader_imports
                 COMMAND "${CMAKE_COMMAND}" -E echo "SKIP: needs the Vulkan backend on an ELF platform")
        set_tests_properties(fuse_rp_volk_no_loader_imports PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
    endif()
    set_tests_properties(fuse_rp_volk_no_loader_imports PROPERTIES SKIP_RETURN_CODE 77 LABELS "gate;vulkan;lint")

    # --- Loader initialisation policy and device adoption (RL-4.1 blockers) ---------------------
    # fuse_rp_volk_explicit_init: links fuse_rhi with FUSE_RHI_VOLK_NO_AUTO_INIT (no static auto-init);
    # proves nothing touches the loader before the first explicit / lazy / host-proc-addr init.
    # fuse_rp_volk_auto_init: the same source on the default link (auto-init before main).
    add_executable(fuse_rp_volk_explicit_init tests/test_rp_volk_explicit_init.cpp)
    target_link_libraries(fuse_rp_volk_explicit_init PRIVATE fuse_rhi)
    set_target_properties(fuse_rp_volk_explicit_init PROPERTIES FUSE_RHI_VOLK_NO_AUTO_INIT ON)
    # fuse_core's platform WSI (X11 / Win32 surface creation) links the loader library itself; then
    # it is mapped at startup regardless of volk, and residency proves nothing (the test still checks
    # volk's state). The PE renderer tree has a null WSI: residency is asserted there.
    set(_fuse_rp_wp02_loader_linked 0)
    if(TARGET fuse_core)
        foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_libs fuse_core ${_prop})
            if(_libs AND "${_libs}" MATCHES "Vulkan::Vulkan")
                set(_fuse_rp_wp02_loader_linked 1)
            endif()
        endforeach()
    endif()
    target_compile_definitions(fuse_rp_volk_explicit_init PRIVATE
        FUSE_RP_LOADER_LINKED_BY_WSI=${_fuse_rp_wp02_loader_linked})
    add_executable(fuse_rp_volk_auto_init tests/test_rp_volk_explicit_init.cpp)
    target_link_libraries(fuse_rp_volk_auto_init PRIVATE fuse_rhi)
    target_compile_definitions(fuse_rp_volk_auto_init PRIVATE FUSE_RP_EXPECT_AUTO_INIT=1)
    set(_fuse_rp_wp02_init_tests "")
    foreach(_mode explicit lazy proc-addr)
        set(_name "fuse_rp_volk_explicit_init_${_mode}")
        string(REPLACE "-" "_" _name "${_name}")
        if(FUSE_VULKAN_BACKEND)
            add_test(NAME ${_name} COMMAND "${_fuse_rp_wp02_lock}" "$<TARGET_FILE:fuse_rp_volk_explicit_init>" --mode ${_mode})
        else()
            add_test(NAME ${_name} COMMAND fuse_rp_volk_explicit_init --mode ${_mode})
        endif()
        list(APPEND _fuse_rp_wp02_init_tests ${_name})
    endforeach()
    if(FUSE_VULKAN_BACKEND AND FUSE_RHI_VOLK_AUTO_INIT)
        add_test(NAME fuse_rp_volk_auto_init COMMAND "${_fuse_rp_wp02_lock}" "$<TARGET_FILE:fuse_rp_volk_auto_init>")
        list(APPEND _fuse_rp_wp02_init_tests fuse_rp_volk_auto_init)
    elseif(NOT FUSE_VULKAN_BACKEND)
        add_test(NAME fuse_rp_volk_auto_init COMMAND fuse_rp_volk_auto_init)
        list(APPEND _fuse_rp_wp02_init_tests fuse_rp_volk_auto_init)
    endif()

    # fuse_rp_vk_adopt: VulkanDevice::adopt of a VulkanDevice (non-owning) and of a raw T0 device
    # (owning); bindless + render-graph smoke test through each adoptee; the original stays usable.
    add_executable(fuse_rp_vk_adopt tests/test_rp_vk_adopt.cpp)
    target_link_libraries(fuse_rp_vk_adopt PRIVATE fuse_rhi)
    if(FUSE_GLSLANG_VALIDATOR)
        set(_fuse_adopt_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/vk_adopt")
        file(MAKE_DIRECTORY "${_fuse_adopt_dir}")
        set(_fuse_adopt_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vk_adopt/rp_vk_adopt.comp")
        set(_fuse_adopt_inc "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common")
        set(_fuse_adopt_spv "${_fuse_adopt_dir}/rp_vk_adopt.comp.spv")
        set(_fuse_adopt_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 "-I${_fuse_adopt_inc}"
                             "${_fuse_adopt_src}" -o "${_fuse_adopt_spv}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _fuse_adopt_cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_fuse_adopt_spv}")
        endif()
        add_custom_command(OUTPUT "${_fuse_adopt_spv}" ${_fuse_adopt_cmds}
            DEPENDS "${_fuse_adopt_src}" "${_fuse_adopt_inc}/bindless.glsl"
            COMMENT "glslangValidator rp_vk_adopt.comp -> rp_vk_adopt.comp.spv"
            VERBATIM)
        add_custom_target(fuse_rp_vk_adopt_shaders DEPENDS "${_fuse_adopt_spv}")
        add_dependencies(fuse_rp_vk_adopt fuse_rp_vk_adopt_shaders)
        target_compile_definitions(fuse_rp_vk_adopt PRIVATE FUSE_RP_VK_ADOPT_SHADER="${_fuse_adopt_spv}")
    endif()
    if(FUSE_VULKAN_BACKEND)
        add_test(NAME fuse_rp_vk_adopt COMMAND "${_fuse_rp_wp02_lock}" "$<TARGET_FILE:fuse_rp_vk_adopt>")
    else()
        add_test(NAME fuse_rp_vk_adopt COMMAND fuse_rp_vk_adopt)
    endif()
    set_tests_properties(${_fuse_rp_wp02_init_tests} fuse_rp_vk_adopt PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        LABELS "gate;vulkan")
endif()
