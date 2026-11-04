// Native startup gate (plan §6: hello-world app, launch to first presented frame, desktop <= 150 ms). Unlike
// fuse_app_hello_startup (in-process, headless CPU present) this launches the real hello_app process several times;
// each run opens an OS window with a real presenter and reports the time from process creation to its first
// presented frame (hello_app --startup-probe). Two series, median of each:
//   * software present (--no-vulkan: CPU raster + GDI blit on Win32) against the plan budget (--budget-ms, 150)
//   * the default Vulkan swapchain presenter against a regression ceiling (--vulkan-budget-ms, 0 = report only).
//     Vulkan instance / device / swapchain creation alone exceeds 150 ms on desktop drivers (see docs/ui/README.md).
//   fuse_app_native_startup <path to hello_app> [--runs=5] [--budget-ms=150] [--vulkan-budget-ms=2000]
// Exit 77 when hello_app gets no OS window at all (no display).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace {
struct Series {
    std::string presenter;
    std::vector<double> ms;
    double median = -1.0;
    bool failed = false;
};

Series run(const std::string& exe, const char* extra, int runs) {
    Series s;
#if defined(_WIN32)
    const std::string cmd = "\"\"" + exe + "\" --startup-probe " + extra + "\""; // cmd /c strips the outer quotes
#else
    const std::string cmd = "'" + exe + "' --startup-probe " + extra;
#endif
    for (int r = 0; r < runs; ++r) {
        std::FILE* p = popen(cmd.c_str(), "r");
        if (!p) { s.failed = true; return s; }
        char line[512];
        double v = -1.0;
        while (std::fgets(line, sizeof line, p)) {
            if (const char* f = std::strstr(line, "first_frame_ms=")) v = std::atof(f + 15);
            if (const char* k = std::strstr(line, "presenter=")) s.presenter.assign(k + 10, std::strcspn(k + 10, " \r\n"));
            std::fputs(line, stdout);
        }
        const int status = pclose(p);
        if (s.presenter == "headless") return s;
        if (v < 0.0 || status != 0) {
            std::fprintf(stderr, "  run %d (%s): no first_frame_ms (status %d)\n", r, extra, status);
            s.failed = true;
            return s;
        }
        s.ms.push_back(v);
    }
    std::vector<double> sorted = s.ms;
    std::sort(sorted.begin(), sorted.end());
    s.median = sorted[sorted.size() / 2];
    return s;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fuse_app_native_startup <hello_app> [--runs=N] [--budget-ms=MS] [--vulkan-budget-ms=MS]\n");
        return 2;
    }
    int runs = 5;
    double budget = 150.0, vkBudget = 2000.0;
    for (int i = 2; i < argc; ++i) {
        if (std::strncmp(argv[i], "--runs=", 7) == 0) runs = std::max(1, std::atoi(argv[i] + 7));
        if (std::strncmp(argv[i], "--budget-ms=", 12) == 0) budget = std::atof(argv[i] + 12);
        if (std::strncmp(argv[i], "--vulkan-budget-ms=", 19) == 0) vkBudget = std::atof(argv[i] + 19);
    }
    const Series sw = run(argv[1], "--no-vulkan", runs);
    const Series vk = run(argv[1], "", runs);
    if (sw.failed || vk.failed) return 1;
    int fail = 0;
    bool any = false;
    if (sw.presenter != "headless") {
        any = true;
        std::printf("fuse_app_native_startup: software present (%s): median %.1f ms, budget %.0f ms\n", sw.presenter.c_str(),
                    sw.median, budget);
        if (sw.median > budget) { std::fprintf(stderr, "  software present over budget\n"); ++fail; }
    } else {
        std::printf("fuse_app_native_startup: no software presenter on this platform (skipped that series)\n");
    }
    if (vk.presenter == "vulkan") {
        any = true;
        std::printf("fuse_app_native_startup: Vulkan swapchain present: median %.1f ms, ceiling %.0f ms (plan budget %.0f ms)\n",
                    vk.median, vkBudget, budget);
        if (vkBudget > 0.0 && vk.median > vkBudget) { std::fprintf(stderr, "  Vulkan present over its ceiling\n"); ++fail; }
    } else if (vk.presenter != "headless") {
        std::printf("fuse_app_native_startup: default presenter is %s (no Vulkan)\n", vk.presenter.c_str());
    }
    if (!any) {
        std::printf("fuse_app_native_startup: skipped (no OS window: headless presenter)\n");
        return 77;
    }
    if (fail) return 1;
    std::printf("fuse_app_native_startup: OK\n");
    return 0;
}
