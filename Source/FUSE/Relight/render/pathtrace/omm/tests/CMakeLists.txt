# FUSE Relight RL-5.6 opacity micromap tests (test_omm.cpp): rl_omm_<suite> (gate;relight; native, PE through the Wine
# emulator in the MinGW tree): index (the spec ordering: bijection, golden level-3 table, curve continuity, hierarchy,
# edges), build (special indices, data layout, usage counts, sharing), equivalence (4-state micromap + any-hit ==
# alpha test on every sampled hit; 2-state loss reported), traversal (closest hit through stacked alpha-tested
# geometry: micromap path == alpha-test path bit for bit, any-hit calls saved), zero_alloc (lookups).
# The device capability gate (VK_EXT_opacity_micromap absent on Lavapipe -> any-hit fallback) runs in the Vulkan test
# of render/volumetrics (rl_pe.vol_vk*, which already holds a device).
add_executable(fuse_relight_omm_tests test_omm.cpp)
target_link_libraries(fuse_relight_omm_tests PRIVATE fuse_relight_omm)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_omm_tests)
else()
    set_target_properties(fuse_relight_omm_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
foreach(_suite index build equivalence traversal zero_alloc)
    add_test(NAME rl_omm_${_suite} COMMAND fuse_relight_omm_tests ${_suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_omm_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 600)
endforeach()
