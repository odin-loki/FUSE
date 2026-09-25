# AssetGen (python, stdlib only): FUSE_ASSET_PLAN W0.9 generator skeleton — recipe schema, seed
# plumbing, content-addressed cache, licence records. ctest `asset_determinism` (labels asset;gate):
# same recipe + seed -> byte-identical outputs, different seed -> different bytes (§5.3).
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME asset_determinism
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_asset_determinism.py"
                --work "${CMAKE_CURRENT_BINARY_DIR}/asset_determinism")
    set_tests_properties(asset_determinism PROPERTIES
        LABELS "asset;gate;tools;python"
        TIMEOUT 600
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
