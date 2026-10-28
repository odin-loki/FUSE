// rl_options_exit: regression for static-destruction order at process exit. Options, layer handles
// and the option registries live in statics across translation units; an option left dirty at exit
// (and a handle released by its static destructor) must not touch destroyed registries. Run under
// ASan/UBSan this fails with a heap-use-after-free if the shared state is destroyed first.

#include <fuse/core/temp_path.hpp>
#include <fuse/relight/options/options.hpp>
#include <fuse/relight/options/options_exit.hpp>
#include <fuse/relight/options/shutdown_phase.hpp>

#include <cstdio>

namespace {

using namespace fuse::relight::options;

// No min/max and no callback, so nothing touches the dirty-options map during static
// initialization: the map is first used in main(), i.e. after these options were constructed.
struct ExitOptions {
    FUSE_RELIGHT_OPTION("rtx.exittest", std::int32_t, dirtyAtExit, 1, "Left dirty at process exit");
    FUSE_RELIGHT_OPTION("rtx.exittest", HashSet, dirtyHashesAtExit, {}, "Hash set left dirty at process exit");
};

// Destroyed at exit, after main(): drops the last reference to a dynamic layer.
OptionLayerHandle s_heldLayer;

// Phases 0 through 3 inclusive are valid. No allocation.
bool testShutdownPhaseOk() {
    if (!shutdown_phase_ok(0)) {
        return false;
    }
    if (!shutdown_phase_ok(3)) {
        return false;
    }
    if (shutdown_phase_ok(-1)) {
        return false;
    }
    if (shutdown_phase_ok(4)) {
        return false;
    }
    return true;
}

// Saved is 0, Cancelled is 1, Invalid is 2. Ok shares Saved's success status.
bool testOptionsExitCode() {
    if (options_exit_code(OptionsExit::Ok) != 0) {
        return false;
    }
    if (options_exit_code(OptionsExit::Saved) != 0) {
        return false;
    }
    if (options_exit_code(OptionsExit::Cancelled) != 1) {
        return false;
    }
    if (options_exit_code(OptionsExit::Invalid) != 2) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!testShutdownPhaseOk()) {
        std::printf("rl_options_exit: testShutdownPhaseOk: FAILED\n");
        return 1;
    }
    setEnvironmentVariable(kDxvkConfEnvVar, "");
    setEnvironmentVariable(kRtxConfEnvVar, "");
    OptionSystemDesc desc;
    desc.baseDirectory = fuse::test::makeUniqueTempDir("rl_options_exit").string();
    desc.exeName = "rl_options_exit.exe";
    OptionSystem::initialize(desc);

    s_heldLayer = OptionManager::acquireLayer("", {kDefaultDynamicLayerPriority, "ExitLayer"});
    ExitOptions::dirtyAtExit.setDeferred(2, s_heldLayer.get());
    ExitOptions::dirtyHashesAtExit.addHash(0x1234, s_heldLayer.get());
    // Deliberately no applyPendingValues() and no shutdown(): both options stay dirty.
    const bool dirty = ExitOptions::dirtyAtExitObject().isDirty() && ExitOptions::dirtyHashesAtExitObject().isDirty();
    std::printf("rl_options_exit: options dirty at exit: %s\n", dirty ? "yes" : "no");
    if (!testOptionsExitCode()) {
        std::printf("rl_options_exit: testOptionsExitCode: FAILED\n");
        return 1;
    }
    return dirty ? 0 : 1;
}
