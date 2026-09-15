# fuse_compute — Track B CUDA compute scaffolding (B2.7)

set(FUSE_COMPUTE_PUBLIC_HEADERS
    include/fuse/compute/ray_march.hpp
    include/fuse/compute/ray_march_job.hpp
)

set(FUSE_COMPUTE_SOURCES
    src/ray_march_cpu.cpp
    src/ray_march_host.cpp
    src/ray_march_job.cpp
)

if(FUSE_CUDA_BACKEND)
    list(APPEND FUSE_COMPUTE_SOURCES
        src/ray_march_cuda_host.cpp
        kernels/ray_march.cu
    )
endif()

add_library(fuse_compute STATIC
    ${FUSE_COMPUTE_SOURCES}
    ${FUSE_COMPUTE_PUBLIC_HEADERS}
)

add_library(fuse::compute ALIAS fuse_compute)

target_include_directories(fuse_compute
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_compute PUBLIC fuse_core)
target_compile_features(fuse_compute PUBLIC cxx_std_17)

if(FUSE_CUDA_BACKEND)
    target_link_libraries(fuse_compute PUBLIC CUDA::cudart)
    set_target_properties(fuse_compute PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    message(STATUS "FUSE: fuse_compute using CUDA toolkit")
else()
    message(STATUS "FUSE: fuse_compute built in CPU reference stub mode (no CUDA toolkit)")
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
