#include <cstdio>
#include <cstdlib>

extern int test_camera_main();
extern int test_scene_serialiser_main();

int main() {
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
