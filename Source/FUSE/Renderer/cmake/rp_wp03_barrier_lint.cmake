# WP-0.3 exit test (b): "a pass can be added without writing any manual barrier".
# cmake -DFUSE_LINT_ROOT=<Source/FUSE> -P rp_wp03_barrier_lint.cmake
#
# Fails when a vkCmdPipelineBarrier / vkCmdPipelineBarrier2 / vkCmdPipelineBarrier2KHR call
# appears in module sources (<module>/src/**, <module>/kernels/**) outside Renderer/src/rg/ and the
# allow-list below. Tests, layers and headers are not scanned.
#
# Allow-list — immediate-mode command buffers that run outside any render graph:
#   Renderer/src/vk/upload_queue.cpp     async upload queue (its own transfer submissions, QFOT)
#   Renderer/src/resource_manager.cpp    one-shot texture upload / mip generation / readback
#   Renderer/src/vk/image_readback.cpp   one-shot image readback helper
# New entries need a reason here and a review by the WP-0.3 owner.
if(NOT FUSE_LINT_ROOT)
    message(FATAL_ERROR "FUSE_LINT_ROOT is required")
endif()
set(_allow
    "Renderer/src/vk/upload_queue.cpp"
    "Renderer/src/resource_manager.cpp"
    "Renderer/src/vk/image_readback.cpp")

file(GLOB_RECURSE _files RELATIVE "${FUSE_LINT_ROOT}"
     "${FUSE_LINT_ROOT}/*/src/*.cpp" "${FUSE_LINT_ROOT}/*/src/*.cc" "${FUSE_LINT_ROOT}/*/src/*.c"
     "${FUSE_LINT_ROOT}/*/src/*.hpp" "${FUSE_LINT_ROOT}/*/src/*.h" "${FUSE_LINT_ROOT}/*/src/*.inl"
     "${FUSE_LINT_ROOT}/*/src/*.cu" "${FUSE_LINT_ROOT}/*/kernels/*.cu" "${FUSE_LINT_ROOT}/*/kernels/*.cpp")
set(_violations "")
set(_scanned 0)
foreach(_rel IN LISTS _files)
    math(EXPR _scanned "${_scanned} + 1")
    if(_rel MATCHES "^Renderer/src/rg/")
        continue()
    endif()
    list(FIND _allow "${_rel}" _allowed)
    if(NOT _allowed EQUAL -1)
        continue()
    endif()
    file(STRINGS "${FUSE_LINT_ROOT}/${_rel}" _hits REGEX "vkCmdPipelineBarrier(2|2KHR)?[ \t]*\\(")
    list(LENGTH _hits _hit_count)
    if(_hit_count GREATER 0)
        # Report the call site, not its argument list (';' would split the CMake list).
        list(APPEND _violations "${_rel} (${_hit_count} call site(s))")
    endif()
endforeach()
if(_scanned EQUAL 0)
    message(FATAL_ERROR "barrier lint scanned no files under ${FUSE_LINT_ROOT}")
endif()
list(LENGTH _violations _count)
if(_count GREATER 0)
    string(REPLACE ";" "\n  " _list "${_violations}")
    message(FATAL_ERROR "manual pipeline barriers outside src/rg/ (${_count}); declare the accesses on an "
                        "rg::Graph pass instead:\n  ${_list}")
endif()
message(STATUS "barrier lint: ${_scanned} files scanned, no manual vkCmdPipelineBarrier outside src/rg/ + allow-list")
