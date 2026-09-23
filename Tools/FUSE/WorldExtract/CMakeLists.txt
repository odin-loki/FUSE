# WorldExtract (python): CPU-only synthetic end-to-end test of the world-model video -> 3D world
# extractor. SKIPs (exit 77) when the core python deps (requirements.txt) are not importable.
# Point FUSE_WORLDEXTRACT_PYTHON at a venv interpreter that has them, e.g.
#   cmake -DFUSE_WORLDEXTRACT_PYTHON=$HOME/.venvs/worldextract/bin/python ...
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(FUSE_WORLDEXTRACT_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH
        "Python interpreter with Tools/FUSE/WorldExtract/requirements.txt installed")
    set(_fuse_wx_cook)
    if(TARGET fuse_cook)
        set(_fuse_wx_cook --fuse-cook "$<TARGET_FILE:fuse_cook>")
    endif()
    add_test(NAME fuse_world_extract_synthetic
        COMMAND "${FUSE_WORLDEXTRACT_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_synthetic.py"
                --work "${CMAKE_CURRENT_BINARY_DIR}/fuse_world_extract_synthetic" ${_fuse_wx_cook})
    set_tests_properties(fuse_world_extract_synthetic PROPERTIES
        LABELS "tools;python"
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        ENVIRONMENT "OMP_NUM_THREADS=4;PYTHONDONTWRITEBYTECODE=1")
endif()
