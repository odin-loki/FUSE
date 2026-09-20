add_executable(fuse_import fuse_import.cpp)
add_executable(fuse_cook fuse_cook.cpp Cook/fuselevel_cook_stub.cpp)
add_executable(fuse_convert fuse_convert.cpp)

target_link_libraries(fuse_import PRIVATE fuse_project fuse_core)
target_link_libraries(fuse_cook PRIVATE fuse_project fuse_world_converter fuse_core)
target_link_libraries(fuse_convert PRIVATE fuse_world_converter fuse_core)
target_include_directories(fuse_cook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

include("${CMAKE_SOURCE_DIR}/cmake/FuseCxx23.cmake")
fuse_apply_cxx23(fuse_import)
fuse_apply_cxx23(fuse_cook)
fuse_apply_cxx23(fuse_convert)

if(COMMAND fuse_apply_asan)
    fuse_apply_asan(fuse_import)
    fuse_apply_asan(fuse_cook)
    fuse_apply_asan(fuse_convert)
endif()
