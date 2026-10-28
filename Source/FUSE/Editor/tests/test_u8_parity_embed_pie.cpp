#include <fuse/core/init.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/pie_fps.hpp>
#include <fuse/project/loader.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct ParityDemoCase {
    const char* demoId;
    bool expectsWorldLoad;
};

void testParityEmbedProjectLoad(const ParityDemoCase& demo) {
    const std::string projectRoot = std::string("Samples/unification/") + demo.demoId;
    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectRoot);
    expectTrue(project.status == fuse::project::LoadStatus::Ok,
               (std::string(demo.demoId) + " project.json loads").c_str());

    fuse::editor::EditorHost host;
    host.runtimeViewport().setProjectRoot(projectRoot);
    host.runtimeViewport().setProjectLabel(demo.demoId);

    fuse::editor::EditorCommand start;
    start.kind = fuse::editor::CommandKind::StartPlay;
    host.postFromUi(std::move(start));

    for (fuse::u32 tick = 0; tick < 3u; ++tick) {
        host.gameTick();
    }

    const fuse::editor::RuntimeEmbedSession& session = host.runtimeViewport().embedSession();
    expectTrue(session.headlessPresentTicks >= 1u,
               (std::string(demo.demoId) + " embed headless present ticks").c_str());
    expectTrue(!session.wsiBackendName.empty(),
               (std::string(demo.demoId) + " embed WSI backend recorded").c_str());

    if (demo.expectsWorldLoad) {
        expectTrue(session.worldLoaded,
                   (std::string(demo.demoId) + " embed world loaded from parity project").c_str());
        expectTrue(session.worldEntityCount > 0u,
                   (std::string(demo.demoId) + " embed world entity count > 0").c_str());
    }

    expectTrue(host.editorState().playing,
               (std::string(demo.demoId) + " PIE active after StartPlay").c_str());
    expectTrue(host.playSession().isActive(),
               (std::string(demo.demoId) + " play session active").c_str());

    fuse::editor::EditorCommand stop;
    stop.kind = fuse::editor::CommandKind::StopPlay;
    host.postFromUi(std::move(stop));
    host.gameTick();

    expectTrue(!host.editorState().playing,
               (std::string(demo.demoId) + " PIE stopped after StopPlay").c_str());
    expectTrue(!host.playSession().isActive(),
               (std::string(demo.demoId) + " play session stopped").c_str());
}

void testPieFpsOk() {
    expectTrue(fuse::editor::pie_fps_ok(30), "30 fps is a PIE rate");
    expectTrue(fuse::editor::pie_fps_ok(60), "60 fps is a PIE rate");
    expectTrue(!fuse::editor::pie_fps_ok(15), "15 fps is not a PIE rate");
    expectTrue(!fuse::editor::pie_fps_ok(0), "0 fps is not a PIE rate");
}

void testAllParityDemosEmbedPie() {
    const std::vector<ParityDemoCase> demos = {
        {"demo_3d_empty", true},
        {"demo_2d_sprites", false},
        {"demo_ai_bt", true},
        {"demo_timeline", true},
        {"demo_fx", true},
        {"demo_adventure_stub", true},
        {"demo_hybrid_hud", false},
    };

    for (const ParityDemoCase& demo : demos) {
        testParityEmbedProjectLoad(demo);
    }
}

} // namespace

int main() {
    testPieFpsOk();
    fuse::core::initialize();
    testAllParityDemosEmbedPie();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_u8_parity_embed_pie_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_u8_parity_embed_pie_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

#include <fuse/editor/embed_parity.hpp>

namespace {

void testEmbedParityCounter() {
    const fuse::editor::EmbedParity matched{4, 4, 10, 10};
    expectTrue(fuse::editor::embed_parity_ok(matched),
               "4/4 entities and 10/10 components is ok");

    const fuse::editor::EmbedParity entityMismatch{4, 3, 10, 10};
    expectTrue(!fuse::editor::embed_parity_ok(entityMismatch), "4/3 entities is not ok");
    expectTrue(fuse::editor::embed_parity_delta(entityMismatch) == 1, "4/3 entity delta is 1");

    const fuse::editor::EmbedParity negativeEntities{-1, -1, 10, 10};
    expectTrue(!fuse::editor::embed_parity_ok(negativeEntities),
               "negative entity counts are not ok");

    const fuse::editor::EmbedParity componentsBelow{4, 4, 3, 3};
    expectTrue(!fuse::editor::embed_parity_ok(componentsBelow),
               "component counts below entity counts are not ok");
}

// Startup registration so this check runs with the rest of the binary.
struct EmbedParityCounterRegistration {
    EmbedParityCounterRegistration() { testEmbedParityCounter(); }
};

[[maybe_unused]] const EmbedParityCounterRegistration g_embedParityCounterRegistration{};

} // namespace
