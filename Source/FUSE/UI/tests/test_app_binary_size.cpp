// Binary size gate (plan §6: hello-world app, `app` profile, full text; measure, then hold within +10 %).
//   fuse_app_binary_size --file=<hello_app> --budget=<bytes> [--profile=<text>]
// The budget is per toolchain and set in tests/CMakeLists.txt from a measured baseline + 10 %; a toolchain without a
// recorded baseline prints the size and exits 77. Sizes are of the shipped file: MSVC Release PE (symbols live in the
// separate PDB), or a stripped copy on ELF toolchains (CMakeLists passes the stripped file).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    std::string file, profile;
    double budget = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--file=", 7) == 0) file = argv[i] + 7;
        if (std::strncmp(argv[i], "--budget=", 9) == 0) budget = std::atof(argv[i] + 9);
        if (std::strncmp(argv[i], "--profile=", 10) == 0) profile = argv[i] + 10;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        std::fprintf(stderr, "fuse_app_binary_size: cannot stat %s\n", file.c_str());
        return 1;
    }
    std::printf("fuse_app_binary_size: %s = %llu bytes (%.1f KiB)%s%s\n", file.c_str(), static_cast<unsigned long long>(size),
                double(size) / 1024.0, profile.empty() ? "" : ", ", profile.c_str());
    if (budget <= 0.0) {
        std::printf("fuse_app_binary_size: skipped (no baseline recorded for this toolchain)\n");
        return 77;
    }
    std::printf("  budget %.0f bytes (%.1f KiB), %.1f %% used\n", budget, budget / 1024.0, 100.0 * double(size) / budget);
    if (double(size) > budget) {
        std::fprintf(stderr, "fuse_app_binary_size: over budget\n");
        return 1;
    }
    std::printf("fuse_app_binary_size: OK\n");
    return 0;
}
