// Binary size gates (plan §6, measure then hold within +10 %): fuse_app_binary_size (hello-world app, `app` profile)
// and fuse_ui_hud_binary_size (the HUD sample, `hud` profile with cooked text).
//   fuse_app_binary_size --file=<binary> --budget=<bytes> [--profile=<text>] [--name=<gate>]
// The budget is per toolchain and set in tests/CMakeLists.txt from a measured baseline + 10 %; a toolchain without a
// recorded baseline prints the size and exits 77. Sizes are of the shipped file: MSVC Release PE (symbols live in the
// separate PDB), or a stripped copy on ELF toolchains (CMakeLists passes the stripped file).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    std::string file, profile, name = "fuse_app_binary_size";
    double budget = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--file=", 7) == 0) file = argv[i] + 7;
        if (std::strncmp(argv[i], "--budget=", 9) == 0) budget = std::atof(argv[i] + 9);
        if (std::strncmp(argv[i], "--profile=", 10) == 0) profile = argv[i] + 10;
        if (std::strncmp(argv[i], "--name=", 7) == 0) name = argv[i] + 7;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        std::fprintf(stderr, "%s: cannot stat %s\n", name.c_str(), file.c_str());
        return 1;
    }
    std::printf("%s: %s = %llu bytes (%.1f KiB)%s%s\n", name.c_str(), file.c_str(), static_cast<unsigned long long>(size),
                double(size) / 1024.0, profile.empty() ? "" : ", ", profile.c_str());
    if (budget <= 0.0) {
        std::printf("%s: skipped (no baseline recorded for this toolchain / profile)\n", name.c_str());
        return 77;
    }
    std::printf("  budget %.0f bytes (%.1f KiB), %.1f %% used\n", budget, budget / 1024.0, 100.0 * double(size) / budget);
    if (double(size) > budget) {
        std::fprintf(stderr, "%s: over budget\n", name.c_str());
        return 1;
    }
    std::printf("%s: OK\n", name.c_str());
    return 0;
}
