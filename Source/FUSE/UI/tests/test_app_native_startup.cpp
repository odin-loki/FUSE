// Native startup gate (plan §6: hello-world app, launch to first presented frame, desktop <= 150 ms). Unlike
// fuse_app_hello_startup (in-process, headless CPU present) this launches the real hello_app process several times;
// each run opens an OS window with a real presenter and reports the time from process creation to its first
// presented frame (hello_app --startup-probe). Series, median of each:
//   * default (asyncVulkan): first frame through the software presenter while Vulkan starts on a worker — within the
//     plan budget (--budget-ms, 150); when Vulkan exists every run must then switch to the Vulkan presenter (median
//     switch time within --vulkan-budget-ms, 2000) and the switched window must equal the CPU raster (max error 0)
//   * --no-vulkan (software presenter only): within the plan budget
//   * --sync-vulkan (show() blocks for instance / device / swapchain): reported, held to the Vulkan ceiling
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
struct Run {
    double first = -1.0, switched = -1.0;
    std::string presenter, final;
    int maxErr = -1;
};
struct Series {
    std::vector<Run> runs;
    bool failed = false, headless = false;
    double median(double Run::*f) const {
        std::vector<double> v;
        for (const Run& r : runs)
            if (r.*f >= 0.0) v.push_back(r.*f);
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
};

std::string word(const char* line, const char* key) {
    const char* k = std::strstr(line, key);
    if (!k) return {};
    k += std::strlen(key);
    return std::string(k, std::strcspn(k, " \r\n"));
}

Series run(const std::string& exe, const char* extra, int runs) {
    Series s;
#if defined(_WIN32)
    const std::string cmd = "\"\"" + exe + "\" --startup-probe " + extra + "\""; // cmd /c strips the outer quotes
#else
    const std::string cmd = "'" + exe + "' --startup-probe " + extra;
#endif
    for (int i = 0; i < runs; ++i) {
        std::FILE* p = popen(cmd.c_str(), "r");
        if (!p) { s.failed = true; return s; }
        char line[512];
        Run r;
        while (std::fgets(line, sizeof line, p)) {
            if (std::strstr(line, "first_frame_ms=")) {
                r.first = std::atof(word(line, "first_frame_ms=").c_str());
                r.presenter = word(line, "presenter=");
            }
            if (std::strstr(line, "switch_ms=")) {
                r.switched = std::atof(word(line, "switch_ms=").c_str());
                r.final = word(line, "final=");
                r.maxErr = std::atoi(word(line, "max_err=").c_str());
            }
            std::printf("    %s", line);
        }
        const int status = pclose(p);
        if (r.presenter == "headless") { s.headless = true; return s; }
        if (r.first < 0.0 || r.final.empty() || status != 0) {
            std::fprintf(stderr, "  run %d (%s): incomplete probe output (status %d)\n", i, extra, status);
            s.failed = true;
            return s;
        }
        s.runs.push_back(r);
    }
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
    const Series def = run(argv[1], "", runs);
    if (def.headless) {
        std::printf("fuse_app_native_startup: skipped (no OS window: headless presenter)\n");
        return 77;
    }
    const Series sw = run(argv[1], "--no-vulkan", runs);
    const Series sync = run(argv[1], "--sync-vulkan", runs);
    if (def.failed || sw.failed || sync.failed) return 1;
    int fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "  FAIL: %s\n", what); ++fail; }
    };

    const bool vulkan = std::any_of(def.runs.begin(), def.runs.end(), [](const Run& r) { return r.final == "vulkan"; });
    const double first = def.median(&Run::first);
    std::printf("fuse_app_native_startup: default presenter: first frame (%s) median %.1f ms, budget %.0f ms\n",
                def.runs[0].presenter.c_str(), first, budget);
    check(first <= budget, "default configuration: first frame over budget");
    if (vulkan) {
        const double sw2vk = def.median(&Run::switched);
        int worst = 0, switchedRuns = 0;
        for (const Run& r : def.runs) {
            worst = std::max(worst, r.maxErr < 0 ? 255 : r.maxErr);
            switchedRuns += r.final == "vulkan" ? 1 : 0;
        }
        std::printf("  switch to Vulkan in %d / %zu runs, median at %.1f ms (ceiling %.0f ms); switched frame vs CPU raster: "
                    "max channel error %d\n", switchedRuns, def.runs.size(), sw2vk, vkBudget, worst);
        check(switchedRuns == int(def.runs.size()), "a run did not switch to the Vulkan presenter");
        check(sw2vk >= 0.0 && sw2vk <= vkBudget, "switch to Vulkan over its ceiling");
        check(worst == 0, "the first Vulkan frame differs from the CPU raster");
    } else {
        std::printf("  no Vulkan presenter here (stays %s)\n", def.runs[0].final.c_str());
    }
    const double swFirst = sw.median(&Run::first);
    std::printf("fuse_app_native_startup: software presenter only: median %.1f ms, budget %.0f ms\n", swFirst, budget);
    check(swFirst <= budget, "software presenter: first frame over budget");
    if (sync.runs[0].presenter == "vulkan") {
        const double s = sync.median(&Run::first);
        std::printf("fuse_app_native_startup: synchronous Vulkan init (--sync-vulkan): median %.1f ms, ceiling %.0f ms\n", s, vkBudget);
        check(s <= vkBudget, "synchronous Vulkan init over its ceiling");
    }
    if (fail) return 1;
    std::printf("fuse_app_native_startup: OK\n");
    return 0;
}
