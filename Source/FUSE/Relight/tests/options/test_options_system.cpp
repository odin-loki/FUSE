// rl_options / system: the Remix layer stack built from files (dxvk.conf, app config, rtx.conf,
// baseGameMod rtx.conf, environment, user.conf), multi-file DXVK_RTX_CONFIG_FILE, [exe] sections,
// dynamic layer files with `-` hash removal, and the byte-stable rtx.conf / user.conf save.

#include "rl_options_test.hpp"

#include <fuse/core/temp_path.hpp>
#include <fuse/relight/options/options.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace rl_options_test {
namespace {

using namespace fuse::relight::options;

enum class SysMode : std::int32_t { Off = 0, Fast = 1, Best = 2 };

struct SysOptions {
    FUSE_RELIGHT_OPTION("rtx.systest", std::int32_t, layered, 0, "Set by every layer of the stack");
    FUSE_RELIGHT_OPTION("rtx.systest", std::int32_t, fromDxvk, 0, "Only in dxvk.conf");
    FUSE_RELIGHT_OPTION("rtx.systest", std::int32_t, fromApp, 0, "Only in the app config");
    FUSE_RELIGHT_OPTION("rtx.systest", std::int32_t, exeOnly, 0, "Only in a [Game.exe] section");
    FUSE_RELIGHT_OPTION_ENV("rtx.systest", std::int32_t, envLayered, 0, "RL_OPTIONS_SYSTEST_ENV",
                            "rtx.conf value overridden by an environment variable");
    FUSE_RELIGHT_OPTION("rtx.systest", HashSet, hashes, {}, "Hash set merged across files");
    FUSE_RELIGHT_OPTION_ARGS("rtx.systest", std::int32_t, quality, 1, "User setting",
                             args.flags = OptionFlags::UserSetting);
    // Every value type, for the save round trip.
    FUSE_RELIGHT_OPTION("rtx.systest", bool, aBool, false, "bool");
    FUSE_RELIGHT_OPTION("rtx.systest", float, aFloat, 0.0f, "float");
    FUSE_RELIGHT_OPTION("rtx.systest", Vec2f, aVec2, Vec2f(), "float2");
    FUSE_RELIGHT_OPTION("rtx.systest", Vec3f, aVec3, Vec3f(), "float3");
    FUSE_RELIGHT_OPTION("rtx.systest", Vec4f, aVec4, Vec4f(), "float4");
    FUSE_RELIGHT_OPTION("rtx.systest", Vec2i, aVec2i, Vec2i(), "int2");
    FUSE_RELIGHT_OPTION("rtx.systest", std::string, aString, "", "string");
    FUSE_RELIGHT_OPTION("rtx.systest", HashVector, aHashVector, {}, "hash vector");
    FUSE_RELIGHT_OPTION("rtx.systest", SysMode, aMode, SysMode::Off, "enum");
    FUSE_RELIGHT_OPTION_FLAG("rtx.systest", std::int32_t, runtimeOnly, 0, OptionFlags::NoSave, "NoSave");
    FUSE_RELIGHT_OPTION("relight.systest", std::int32_t, native, 0, "FUSE-only option");
};

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void apply() { OptionManager::applyPendingValues(nullptr, false); }

OptionSystemDesc descFor(const std::filesystem::path& dir, const char* exe = "Game.exe") {
    OptionSystemDesc desc;
    desc.baseDirectory = dir.string();
    desc.exeName = exe;
    return desc;
}

// ----------------------------------------------------------------------------

void testLayerStackFromFiles() {
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_options_stack");
    writeFile(dir / "dxvk.conf",
              "d3d9.presentInterval = 1\n"
              "rtx.systest.fromDxvk = 1\n"
              "rtx.systest.layered = 1\n");
    writeFile(dir / "rtx.conf",
              "rtx.systest.layered = 3\n"
              "rtx.systest.envLayered = 3\n"
              "rtx.systest.hashes = 0x1, 0x2, 0x3\n"
              "rtx.baseGameModPathRegex = mods/(.*)\n");
    writeFile(dir / "mod" / "rtx.conf", "rtx.systest.layered = 4\n");
    writeFile(dir / "user.conf", "rtx.systest.quality = 7\n");
    writeFile(dir / "indoor.conf", "rtx.systest.hashes = -0x2, 0x4\nrtx.systest.layered = 100\n");

    OptionSystemDesc desc = descFor(dir);
    desc.appConfig.set("rtx.systest.fromApp", "2");
    desc.appConfig.set("rtx.systest.layered", "2");
    std::string resolverSaw;
    desc.baseGameModPathResolver = [&](const OptionConfig& merged) {
        const std::string* regex = merged.find("rtx.baseGameModPathRegex");
        resolverSaw = regex ? *regex : std::string();
        return (dir / "mod").string();
    };
    RL_CHECK(setEnvironmentVariable("RL_OPTIONS_SYSTEST_ENV", "5"));
    const OptionConfig& merged = OptionSystem::initialize(desc);
    RL_CHECK(setEnvironmentVariable("RL_OPTIONS_SYSTEST_ENV", ""));
    RL_CHECK(OptionSystem::isInitialized());

    // Merged config (for DXVK options): dxvk.conf < app < rtx.conf < baseGameMod.
    RL_CHECK_STR(resolverSaw, "mods/(.*)");
    RL_CHECK(merged.get<std::int32_t>("d3d9.presentInterval", 0) == 1);
    RL_CHECK(merged.get<std::int32_t>("rtx.systest.layered", 0) == 4);
    RL_CHECK(!merged.contains("rtx.systest.quality")); // user.conf is not part of it

    RL_CHECK(SysOptions::fromDxvk() == 1);
    RL_CHECK(SysOptions::fromApp() == 2);
    RL_CHECK(SysOptions::layered() == 4); // baseGameMod beats rtx.conf, app config and dxvk.conf
    RL_CHECK(SysOptions::envLayered() == 5); // environment beats rtx.conf
    RL_CHECK(SysOptions::quality() == 7);
    RL_CHECK((SysOptions::hashes() == HashSet{0x1, 0x2, 0x3}));
    RL_CHECK(SysOptions::envLayeredObject().hasValueInLayer(OptionLayer::getEnvironmentLayer()));

    // Walk up the stack: derived < dynamic < user < quality.
    SysOptions::layered.setDeferred(6); // code-driven: Derived layer
    apply();
    RL_CHECK(SysOptions::layered() == 6);
    OptionLayerHandle indoor =
        OptionManager::acquireLayer((dir / "indoor.conf").string(), {kDefaultDynamicLayerPriority, "indoor.conf"});
    apply();
    RL_CHECK(SysOptions::layered() == 100);
    // `-` in a stronger file removes a hash that rtx.conf added.
    RL_CHECK((SysOptions::hashes() == HashSet{0x1, 0x3, 0x4}));
    SysOptions::layered.setDeferred(0xFFFE, OptionLayer::getUserLayer());
    apply();
    RL_CHECK(SysOptions::layered() == 0xFFFE);
    SysOptions::layered.setDeferred(0xFFFF, OptionLayer::getQualityLayer());
    apply();
    RL_CHECK(SysOptions::layered() == 0xFFFF);
    // The user layer holds a developer option now: it is miscategorized there.
    RL_CHECK(OptionLayer::getUserLayer()->countMiscategorizedOptions() == 1);

    // A half-strength dynamic layer: ints need strength >= threshold (0.1 by default).
    indoor.get()->requestBlendStrength(0.05f);
    SysOptions::layeredObject().disableLayerValue(OptionLayer::getQualityLayer());
    SysOptions::layeredObject().disableLayerValue(OptionLayer::getUserLayer());
    apply();
    RL_CHECK(SysOptions::layered() == 6);
    RL_CHECK((SysOptions::hashes() == HashSet{0x1, 0x2, 0x3}));
    indoor.release();
    SysOptions::layeredObject().disableLayerValue(OptionLayer::getDerivedLayer());
    apply();
    RL_CHECK(SysOptions::layered() == 4);

    // Shutdown returns everything to its default.
    OptionSystem::shutdown();
    RL_CHECK(!OptionSystem::isInitialized());
    RL_CHECK(SysOptions::layered() == 0);
    RL_CHECK(SysOptions::quality() == 1);
    RL_CHECK(SysOptions::hashes().empty());
    RL_CHECK(OptionLayer::getRtxConfLayer() == nullptr);
    RL_CHECK(OptionManager::getLayerRegistry().size() == 1); // Default Values only

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testMultipleRtxConfFiles() {
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_options_multi");
    writeFile(dir / "a.conf", "rtx.systest.layered = 10\nrtx.systest.fromApp = 1\n");
    writeFile(dir / "b.conf", "rtx.systest.layered = 20\n");
    const std::string a = (dir / "a.conf").string();
    const std::string b = (dir / "b.conf").string();
    RL_CHECK(setEnvironmentVariable(kRtxConfEnvVar, a + "," + b));
    const OptionConfig merged = OptionSystem::initialize(descFor(dir));
    RL_CHECK(setEnvironmentVariable(kRtxConfEnvVar, ""));

    // Upstream naming: "00_Remix Config" (a), "Remix Config" (b). Names sort, so the FIRST listed
    // file is the strongest layer, while the merged config lets the LAST file win.
    const OptionLayer* first = OptionManager::getLayer({kRtxConfLayerId.priority, "00_Remix Config"});
    const OptionLayer* last = OptionManager::getLayer(OptionLayerKey(kRtxConfLayerId));
    RL_CHECK(first && first->getFilePath() == a);
    RL_CHECK(last && last->getFilePath() == b);
    RL_CHECK(OptionLayer::getRtxConfLayer() == last); // user edits target the last file
    RL_CHECK(SysOptions::layered() == 10);
    RL_CHECK(SysOptions::fromApp() == 1);
    RL_CHECK(merged.get<std::int32_t>("rtx.systest.layered", 0) == 20);
    RL_CHECK(OptionLayer::resolveConfigPaths("RL_OPTIONS_UNSET_PATH_VAR", "x.conf") ==
             std::vector<std::string>{"x.conf"});
    OptionSystem::shutdown();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testExeSections() {
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_options_sections");
    writeFile(dir / "rtx.conf",
              "rtx.systest.layered = 3\n"
              "[Game.exe]\n"
              "rtx.systest.exeOnly = 1\n"
              "[Other.exe]\n"
              "rtx.systest.exeOnly = 2\n"
              "rtx.systest.layered = 30\n");
    OptionSystem::initialize(descFor(dir, "Game.exe"));
    RL_CHECK(SysOptions::exeOnly() == 1);
    RL_CHECK(SysOptions::layered() == 3);
    RL_CHECK_STR(OptionSystem::parseOptions().exeName, "Game.exe");
    OptionSystem::shutdown();
    OptionSystem::initialize(descFor(dir, "Other.exe"));
    RL_CHECK(SysOptions::exeOnly() == 2);
    RL_CHECK(SysOptions::layered() == 30);
    OptionSystem::shutdown();
    OptionSystem::initialize(descFor(dir, "Third.exe"));
    RL_CHECK(SysOptions::exeOnly() == 0);
    OptionSystem::shutdown();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testRtxConfSaveRoundTrip() {
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_options_roundtrip");
    // Canonical rtx.conf: sorted keys, Remix formatting, every value type.
    const std::string canonical =
        "relight.systest.native = 12\n"
        "rtx.systest.aBool = True\n"
        "rtx.systest.aFloat = 0.1\n"
        "rtx.systest.aHashVector = 0x0000000000000003, 0x0000000000000001, 0x0000000000000003\n"
        "rtx.systest.aMode = 2\n"
        "rtx.systest.aString = \"  padded name\"\n"
        "rtx.systest.aVec2 = 1.5, -2\n"
        "rtx.systest.aVec2i = 1920, 1080\n"
        "rtx.systest.aVec3 = 0.333333, 1e-05, 100000\n"
        "rtx.systest.aVec4 = 1, 2, 3, 4\n"
        "rtx.systest.hashes = 0x0000000000000001, 0x8DD6F568BD126398, -0xEEF8EFD4B8A1B2A5\n"
        "rtx.systest.layered = -7\n";
    writeFile(dir / "rtx.conf", canonical);
    OptionSystem::initialize(descFor(dir));
    OptionLayer* rtxConf = OptionLayer::getRtxConfLayer();
    RL_CHECK(rtxConf != nullptr);
    if (!rtxConf) {
        return;
    }
    RL_CHECK(SysOptions::aMode() == SysMode::Best);
    RL_CHECK_STR(SysOptions::aString(), "  padded name");
    RL_CHECK(SysOptions::aVec2i() == Vec2i(1920, 1080));
    RL_CHECK(SysOptions::native() == 12);
    RL_CHECK(!rtxConf->hasUnsavedChanges());

    // load -> save is byte-identical.
    RL_CHECK(rtxConf->save());
    RL_CHECK_STR(readFile(dir / "rtx.conf"), canonical);

    // A user edit goes to rtx.conf (developer option) and saves as exactly one changed line.
    {
        OptionLayerTarget target(OptionEditTarget::User);
        SysOptions::layered.setDeferred(8);
        SysOptions::hashes.addHash(0x2);
    }
    apply();
    RL_CHECK(rtxConf->hasUnsavedChanges());
    RL_CHECK(rtxConf->save());
    RL_CHECK(!rtxConf->hasUnsavedChanges());
    std::string expected = canonical;
    expected.replace(expected.find("rtx.systest.layered = -7"), 24, "rtx.systest.layered = 8");
    expected.replace(expected.find("0x0000000000000001, 0x8DD6"), 20, "0x0000000000000001, 0x0000000000000002, ");
    RL_CHECK_STR(readFile(dir / "rtx.conf"), expected);

    // reload() discards runtime edits.
    SysOptions::layered.setDeferred(9, rtxConf);
    apply();
    RL_CHECK(SysOptions::layered() == 9);
    RL_CHECK(rtxConf->reload());
    apply();
    RL_CHECK(SysOptions::layered() == 8);
    RL_CHECK(!rtxConf->hasUnsavedChanges());
    OptionSystem::shutdown();

    // A fresh start reads the saved file back to the same values, and saves the same bytes.
    OptionSystem::initialize(descFor(dir));
    RL_CHECK(SysOptions::layered() == 8);
    RL_CHECK(SysOptions::hashes.containsHash(0x2));
    RL_CHECK(OptionLayer::getRtxConfLayer()->save());
    RL_CHECK_STR(readFile(dir / "rtx.conf"), expected);
    OptionSystem::shutdown();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testSaveRulesAndIdempotence() {
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_options_saverules");
    const std::string messy =
        "# Mod settings\n"
        "rtx.systest.aFloat=0.10000\r\n"
        "  rtx.systest.aBool = 1\n"
        "rtx.systest.hashes = 0x3,-0x2, 0X1\n"
        "relight.systest.layered = 5\n"      // twin name of rtx.systest.layered
        "rtx.systest.runtimeOnly = 4\n"      // NoSave: applies, never written back
        "rtx.systest.unknownOption = 1\n"    // not a registered option
        "d3d9.maxFrameRate = 60\n"
        "rtx.systest.aString = hello\n";
    writeFile(dir / "rtx.conf", messy);
    OptionSystem::initialize(descFor(dir));
    OptionLayer* rtxConf = OptionLayer::getRtxConfLayer();
    RL_CHECK(SysOptions::layered() == 5);
    RL_CHECK(SysOptions::runtimeOnly() == 4);
    RL_CHECK(SysOptions::aBool());
    // Unknown keys survive the default save, so they are not pending removals.
    RL_CHECK(!rtxConf->hasPendingRemovals());

    // Default (Relight): registered options under their declared names, NoSave dropped, and every
    // key no registered option claims kept, all sorted.
    const std::string preserved =
        "d3d9.maxFrameRate = 60\n"
        "rtx.systest.aBool = True\n"
        "rtx.systest.aFloat = 0.1\n"
        "rtx.systest.aString = hello\n"
        "rtx.systest.hashes = 0x0000000000000001, 0x0000000000000003, -0x0000000000000002\n"
        "rtx.systest.layered = 5\n"
        "rtx.systest.unknownOption = 1\n";
    RL_CHECK(rtxConf->save());
    RL_CHECK_STR(readFile(dir / "rtx.conf"), preserved);
    RL_CHECK(!rtxConf->hasUnsavedChanges());
    // Saving again, after a reload and after a restart, changes nothing.
    RL_CHECK(rtxConf->reload());
    apply();
    RL_CHECK(rtxConf->save());
    RL_CHECK_STR(readFile(dir / "rtx.conf"), preserved);
    OptionSystem::shutdown();
    OptionSystem::initialize(descFor(dir));
    RL_CHECK(OptionLayer::getRtxConfLayer()->save());
    RL_CHECK_STR(readFile(dir / "rtx.conf"), preserved);
    // An edit changes exactly its own line; unknown keys stay put.
    {
        OptionLayerTarget target(OptionEditTarget::User);
        SysOptions::layered.setDeferred(6);
    }
    apply();
    RL_CHECK(OptionLayer::getRtxConfLayer()->save());
    std::string edited = preserved;
    edited.replace(edited.find("rtx.systest.layered = 5"), 23, "rtx.systest.layered = 6");
    RL_CHECK_STR(readFile(dir / "rtx.conf"), edited);
    OptionSystem::shutdown();

    // Strict upstream rule on request: only registered, saveable options are written.
    writeFile(dir / "rtx.conf", messy);
    OptionSystem::initialize(descFor(dir));
    rtxConf = OptionLayer::getRtxConfLayer();
    const std::string strict =
        "rtx.systest.aBool = True\n"
        "rtx.systest.aFloat = 0.1\n"
        "rtx.systest.aString = hello\n"
        "rtx.systest.hashes = 0x0000000000000001, 0x0000000000000003, -0x0000000000000002\n"
        "rtx.systest.layered = 5\n";
    RL_CHECK(rtxConf->save(LayerSaveOptions{false}));
    RL_CHECK_STR(readFile(dir / "rtx.conf"), strict);
    RL_CHECK(rtxConf->reload());
    apply();
    RL_CHECK(rtxConf->save(LayerSaveOptions{false}));
    RL_CHECK_STR(readFile(dir / "rtx.conf"), strict);
    OptionSystem::shutdown();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testUserConfSave() {
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_options_userconf");
    OptionSystem::initialize(descFor(dir));
    {
        OptionLayerTarget target(OptionEditTarget::User);
        SysOptions::quality.setDeferred(3);  // UserSetting -> user.conf
        SysOptions::layered.setDeferred(2);  // developer option -> rtx.conf
    }
    apply();
    RL_CHECK(SysOptions::quality() == 3);
    const OptionLayer* user = OptionLayer::getUserLayer();
    RL_CHECK(user->hasUnsavedChanges());
    RL_CHECK(const_cast<OptionLayer*>(user)->save());
    RL_CHECK_STR(readFile(dir / "user.conf"), "rtx.systest.quality = 3\n");
    RL_CHECK(OptionLayer::getRtxConfLayer()->save());
    RL_CHECK_STR(readFile(dir / "rtx.conf"), "rtx.systest.layered = 2\n");
    // Layers without a file cannot be saved.
    RL_CHECK(!const_cast<OptionLayer*>(OptionLayer::getDerivedLayer())->save());
    OptionSystem::shutdown();

    OptionSystem::initialize(descFor(dir));
    RL_CHECK(SysOptions::quality() == 3);
    RL_CHECK(SysOptions::layered() == 2);
    OptionSystem::shutdown();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testEnvironmentOverrideEdgeCases() {
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_options_env");
    writeFile(dir / "rtx.conf", "rtx.systest.envLayered = 3\n");
    // A malformed value leaves the upstream behaviour: the entry exists in the Environment layer
    // with the type's zero value, and a warning is logged.
    RL_CHECK(setEnvironmentVariable("RL_OPTIONS_SYSTEST_ENV", "banana"));
    OptionSystem::initialize(descFor(dir));
    RL_CHECK(setEnvironmentVariable("RL_OPTIONS_SYSTEST_ENV", ""));
    RL_CHECK(SysOptions::envLayered() == 0);
    OptionSystem::shutdown();
    // Unset: rtx.conf applies.
    OptionSystem::initialize(descFor(dir));
    RL_CHECK(SysOptions::envLayered() == 3);
    RL_CHECK(!SysOptions::envLayeredObject().hasValueInLayer(OptionLayer::getEnvironmentLayer()));
    // Derived (code) values beat the environment layer.
    SysOptions::envLayered.setDeferred(11);
    apply();
    RL_CHECK(SysOptions::envLayered() == 11);
    OptionSystem::shutdown();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace

void runSystemTests() {
    OptionSystem::shutdown();
    static const TestCase kCases[] = {
        {"layerStackFromFiles", testLayerStackFromFiles},
        {"multipleRtxConfFiles", testMultipleRtxConfFiles},
        {"exeSections", testExeSections},
        {"rtxConfSaveRoundTrip", testRtxConfSaveRoundTrip},
        {"saveRulesAndIdempotence", testSaveRulesAndIdempotence},
        {"userConfSave", testUserConfSave},
        {"environmentOverrideEdgeCases", testEnvironmentOverrideEdgeCases},
    };
    runSuite("system", kCases);
}

} // namespace rl_options_test
