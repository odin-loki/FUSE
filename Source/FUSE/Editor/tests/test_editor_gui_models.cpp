// GUI-1 headless models behind the Qt shell: AssetBrowser folder navigation / activation and
// ProfilerPanel frame statistics + graph history.
#include <fuse/editor/asset_browser.hpp>
#include <fuse/editor/profiler_panel.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

void touch(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

void testAssetNavigation() {
    using fuse::editor::AssetBrowser;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "fuse_gui_models_assets";
    fs::remove_all(root);
    touch(root / "worlds" / "main.fuselevel");
    touch(root / "worlds" / "nested" / "deep.lua");
    touch(root / "readme.txt");

    AssetBrowser browser;
    browser.init(root.string().c_str());
    expect(browser.entries().size() == 2u && browser.entries()[0].type == AssetBrowser::AssetType::Folder,
           "assets: root lists folder first");
    expect(!browser.navigateUp(), "assets: navigateUp at root is a no-op");
    expect(browser.activate("missing") == AssetBrowser::ActivateResult::None, "assets: unknown path -> None");
    expect(browser.activate("readme.txt") == AssetBrowser::ActivateResult::Selected &&
               browser.selectedEntry() != nullptr,
           "assets: plain file -> Selected");
    expect(browser.activate("worlds") == AssetBrowser::ActivateResult::EnteredFolder &&
               browser.currentDirectory() == "worlds",
           "assets: folder -> EnteredFolder");
    bool sawScene = false;
    for (const auto& e : browser.entries()) {
        sawScene = sawScene || (e.name == "main.fuselevel" && e.type == AssetBrowser::AssetType::Scene);
    }
    expect(sawScene, "assets: .fuselevel classifies as Scene");
    expect(browser.activate("worlds/main.fuselevel") == AssetBrowser::ActivateResult::OpenScene,
           "assets: scene -> OpenScene");
    expect(fs::path(browser.absolutePath("worlds/main.fuselevel")) == fs::path((root / "worlds" / "main.fuselevel").generic_string()),
           "assets: absolutePath joins the project root");
    expect(browser.activate("worlds/nested") == AssetBrowser::ActivateResult::EnteredFolder, "assets: nested folder");
    expect(browser.navigateUp() && browser.currentDirectory() == "worlds", "assets: Up -> parent");
    expect(browser.navigateUp() && browser.currentDirectory().empty(), "assets: Up -> root");
    fs::remove_all(root);
}

void testProfilerStats() {
    using fuse::editor::ProfilerPanel;
    ProfilerPanel panel;
    expect(panel.cpuStats().frames == 0u && panel.cpuStats().avgMs == 0.f, "profiler: empty stats");
    for (float ms : {10.f, 20.f, 30.f}) {
        ProfilerPanel::FrameProfileData f{};
        f.cpuMs = ms;
        panel.pushFrameData(f);
    }
    const auto s = panel.cpuStats();
    expect(s.frames == 3u && s.minMs == 10.f && s.maxMs == 30.f && std::fabs(s.avgMs - 20.f) < 1e-4f,
           "profiler: min / avg / max");
    expect(std::fabs(s.avgFps - 50.f) < 1e-3f, "profiler: avg fps");
    expect(s.overBudget == 2u, "profiler: frames over the 60 fps budget");
    std::vector<float> hist;
    panel.copyCpuHistory(hist);
    expect(hist == std::vector<float>{10.f, 20.f, 30.f}, "profiler: history oldest first");
    for (unsigned i = 0; i < ProfilerPanel::kHistoryFrames + 5u; ++i) {
        ProfilerPanel::FrameProfileData f{};
        f.cpuMs = static_cast<float>(i);
        panel.pushFrameData(f);
    }
    panel.copyCpuHistory(hist);
    expect(hist.size() == ProfilerPanel::kHistoryFrames && hist.front() == 5.f &&
               hist.back() == static_cast<float>(ProfilerPanel::kHistoryFrames + 4u),
           "profiler: wrapped history stays ordered");
    panel.clearHistory();
    panel.copyCpuHistory(hist);
    expect(hist.empty(), "profiler: clear empties the history");
}

} // namespace

int main() {
    testAssetNavigation();
    testProfilerStats();
    std::printf("gui models: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
