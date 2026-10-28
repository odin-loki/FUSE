#include <fuse/scene/pack_name.hpp>

#include <cstdio>
#include <cstdlib>

extern int test_camera_main();
extern int test_scene_serialiser_main();

namespace {

int testPackNameOk() {
    int failures = 0;
    if (!fuse::scene::pack_name_ok("Pack")) {
        ++failures;
    }
    if (fuse::scene::pack_name_ok("")) {
        ++failures;
    }
    if (fuse::scene::pack_name_ok("1Pack")) {
        ++failures;
    }
    if (fuse::scene::pack_name_ok("Map Pack")) {
        ++failures;
    }
    return failures;
}

} // namespace

int main() {
    const int packFailures = testPackNameOk();
    if (packFailures != 0) {
        std::fprintf(stderr, "fuse_scene_serial_tests: %d failure(s)\n", packFailures);
        return EXIT_FAILURE;
    }

    const int cameraFailures = test_camera_main();
    const int serialiserFailures = test_scene_serialiser_main();
    const int failures = cameraFailures + serialiserFailures;

    if (failures == 0) {
        std::printf("fuse_scene_serial_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_scene_serial_tests: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
