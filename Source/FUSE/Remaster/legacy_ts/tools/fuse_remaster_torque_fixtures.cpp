// fuse_remaster_torque_fixtures (Remaster W1.1): write the synthetic Torque fixtures (torque_fixtures.hpp).
//
//   fuse_remaster_torque_fixtures <out-dir>              write shapes/*.dts, *.dsq, *.png below <out-dir>
//   fuse_remaster_torque_fixtures --manifest             print the generated.sha256 lines
//
// Exit 0 on success, 1 on failure, 2 on usage errors.

#include <fuse/remaster/legacy_ts/torque_fixtures.hpp>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    namespace lt = fuse::remaster::legacy_ts;
    if (argc != 2) {
        std::fprintf(stderr, "usage: fuse_remaster_torque_fixtures <out-dir> | --manifest\n");
        return 2;
    }
    std::string error;
    if (std::string(argv[1]) == "--manifest") {
        const auto files = lt::buildTorqueFixtures(&error);
        if (files.empty()) {
            std::fprintf(stderr, "fuse_remaster_torque_fixtures: %s\n", error.c_str());
            return 1;
        }
        std::fputs(lt::fixtureManifest(files).c_str(), stdout);
        return 0;
    }
    if (!lt::writeTorqueFixtures(argv[1], &error)) {
        std::fprintf(stderr, "fuse_remaster_torque_fixtures: %s\n", error.c_str());
        return 1;
    }
    std::printf("fuse_remaster_torque_fixtures: wrote fixtures to %s\n", argv[1]);
    return 0;
}
