# FUSE product source umbrella — L0 fuse_core and future layers.

add_subdirectory(Core)
add_subdirectory(World2D)
add_subdirectory(World3D)
add_subdirectory(Hybrid)

if(FUSE_BUILD_PROJECT)
    add_subdirectory(Project)
endif()

if(FUSE_BUILD_MODULES)
    add_subdirectory(Modules)
endif()

if(FUSE_BUILD_EDITOR_API)
    add_subdirectory(Editor)
endif()

if(FUSE_BUILD_LEGACY)
    add_subdirectory(Legacy)
endif()

if(FUSE_BUILD_LEGACY AND FUSE_BUILD_SMOKE OR FUSE_BUILD_HYBRID_DEMO OR FUSE_BUILD_PARITY_DEMOS)
    add_subdirectory(Apps)
endif()
