#include <fuse/renderer/denoise/denoise_radius.hpp>

#include <cstdio>
#include <cstdlib>

namespace {
constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// CPU gate. Must run before any GPU skip; a failure exits the process.
int testDenoiseRadiusOk() {
    const int before = g_failures;
    expect(fuse::renderer::denoise_radius_ok(1), "denoise_radius_ok(1) is true");
    expect(fuse::renderer::denoise_radius_ok(8), "denoise_radius_ok(8) is true");
    expect(!fuse::renderer::denoise_radius_ok(0), "denoise_radius_ok(0) is false");
    expect(!fuse::renderer::denoise_radius_ok(9), "denoise_radius_ok(9) is false");
    if (g_failures != before) {
        std::fprintf(stderr, "FAIL: testDenoiseRadiusOk\n");
        return 1;
    }
    return 0;
}
} // namespace

int main() {
    if (const int radiusRc = testDenoiseRadiusOk()) {
        return radiusRc;
    }
#if !defined(FUSE_VULKAN_BACKEND)
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
#else
    return 0;
#endif
}
