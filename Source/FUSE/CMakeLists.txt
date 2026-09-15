# FUSE product source umbrella — L0 fuse_core and future layers.

add_subdirectory(Core)

if(FUSE_BUILD_LEGACY)
    add_subdirectory(Legacy)
endif()

if(FUSE_BUILD_LEGACY AND FUSE_BUILD_SMOKE)
    add_subdirectory(Apps)
endif()
