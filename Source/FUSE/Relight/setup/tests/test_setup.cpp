// FUSE Relight RL-6.3 unit tests (ctest rl_setup_unit): profile JSON and rtx.conf round trips, the profile as
// the app-config option layer (priority against dxvk.conf, rtx.conf, the environment and dynamic layers, hash-set
// `-` removal), discovery by exe name / hash, and the setup assistant's proposals on synthetic capture records.

#include <fuse/core/temp_path.hpp>
#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/options/options.hpp>
#include <fuse/relight/scene/classify/classify_options.hpp>
#include <fuse/relight/setup/profile.hpp>
#include <fuse/relight/setup/profile_discovery.hpp>
#include <fuse/relight/setup/setup_assistant.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace fuse::relight;
using namespace fuse::relight::setup;
using options::HashSet;

int g_checks = 0;
int g_failures = 0;
const char* g_test = "";

#define CHECK(...)                                                                                                 \
    do {                                                                                                           \
        ++g_checks;                                                                                                \
        if (!(__VA_ARGS__)) {                                                                                      \
            ++g_failures;                                                                                          \
            std::fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, g_test, #__VA_ARGS__);             \
        }                                                                                                          \
    } while (0)

#define CHECK_STR(a, b)                                                                                            \
    do {                                                                                                           \
        ++g_checks;                                                                                                \
        const std::string rlA_ = (a), rlB_ = (b);                                                                  \
        if (rlA_ != rlB_) {                                                                                        \
            ++g_failures;                                                                                          \
            std::fprintf(stderr, "FAIL %s:%d [%s]: %s == %s\n  got:      [%s]\n  expected: [%s]\n", __FILE__,     \
                         __LINE__, g_test, #a, #b, rlA_.c_str(), rlB_.c_str());                                    \
        }                                                                                                          \
    } while (0)

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void apply() { options::OptionManager::applyPendingValues(nullptr, false); }

GameProfile sampleProfile() {
    GameProfile p;
    p.name = "Sample \"Game\"";
    p.description = "unit test";
    p.match.exeNames = {"Sample.exe", "sample_dx9.exe"};
    p.match.exeHashes = {0x0123456789ABCDEFull};
    p.options["rtx.skyMinZThreshold"] = "0.5";
    p.options["rtx.preTransformedVerticesIsUI"] = "True";
    p.addTexture("sky", 0xA);
    p.addTexture("sky", 0xB);
    p.addTexture("rtx.uiTextures", 0xC); // option key works as a category name
    p.textures["ignore"].remove(0xD);    // Remix `-hash` opinion
    ProfileSuggestion s;
    s.kind = ProfileSuggestion::Kind::Texture;
    s.category = "decal";
    s.hash = 0xE;
    s.confidence = 0.6;
    s.reason = "blended";
    p.suggestions.push_back(s);
    ProfileSuggestion o;
    o.kind = ProfileSuggestion::Kind::Option;
    o.key = "rtx.orthographicIsUI";
    o.value = "False";
    o.confidence = 0.35;
    o.applied = false;
    o.reason = "why";
    p.suggestions.push_back(o);
    p.notes = {"note one", "line\nbreak"};
    return p;
}

// ---- format --------------------------------------------------------------------------------------------------

void testProfileRoundTrip() {
    const GameProfile p = sampleProfile();
    const std::string text = writeProfile(p);
    std::string error;
    const std::optional<GameProfile> back = parseProfile(text, &error);
    CHECK(back.has_value());
    if (!back) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return;
    }
    CHECK_STR(writeProfile(*back), text); // byte-stable
    CHECK(back->name == p.name);
    CHECK(back->match.exeHashes == p.match.exeHashes);
    CHECK(back->options == p.options);
    CHECK(back->textures.at("sky").positives() == (HashSet{0xA, 0xB}));
    CHECK(back->textures.at("ignore").hasNegative(0xD));
    CHECK(back->suggestions == p.suggestions);
    CHECK(back->notes == p.notes);

    // A hand-written file in any order, with bools and numbers as option values, reads into canonical form.
    const char* hand = R"({"textures": {"ui": ["c", "0xA"]}, "schema": "fuse.relight.profile/1",
        "options": {"rtx.skyMinZThreshold": 0.25, "rtx.orthographicIsUI": false},
        "match": {"exe": "game.exe"}, "name": "hand", "unknown": [1, 2]})";
    const std::optional<GameProfile> h = parseProfile(hand, &error);
    CHECK(h.has_value());
    if (h) {
        CHECK_STR(h->options.at("rtx.skyMinZThreshold"), "0.25");
        CHECK_STR(h->options.at("rtx.orthographicIsUI"), "False");
        CHECK(h->textures.at("ui").positives() == (HashSet{0xA, 0xC}));
        const std::string canon = writeProfile(*h);
        CHECK_STR(writeProfile(*parseProfile(canon)), canon);
    }

    CHECK(!parseProfile(R"({"schema": "other/1"})", &error));
    CHECK(!parseProfile(R"({"schema": "fuse.relight.profile/1", "textures": {"weird": ["0x1"]}})", &error));
    CHECK(error.find("unknown category") != std::string::npos);
    CHECK(!parseProfile(R"({"schema": "fuse.relight.profile/1", "textures": {"sky": ["0xZZ"]}})", &error));
    CHECK(!parseProfile(R"({"schema": "fuse.relight.profile/1", "match": {"xxh3": ["nope"]}})", &error));
    CHECK(!parseProfile(R"({"schema": "fuse.relight.profile/1", "options": {"rtx.a": [1]}})", &error));
    CHECK(!parseProfile(R"({"schema": "fuse.relight.profile/1",)", &error));
}

void testAcceptSuggestions() {
    GameProfile p = sampleProfile();
    CHECK(p.acceptSuggestions("0x0E") == 1);
    CHECK(p.textures.at("decal").hasPositive(0xE));
    CHECK(p.suggestions[0].applied);
    CHECK(p.acceptSuggestions("0xE") == 0); // already applied
    CHECK(p.acceptSuggestions("rtx.orthographicIsUI") == 1);
    CHECK_STR(p.options.at("rtx.orthographicIsUI"), "False");
    CHECK(p.acceptSuggestions("all") == 0);
}

void testRtxConfRoundTrip() {
    const GameProfile p = sampleProfile();
    const std::string conf = exportRtxConf(p);
    CHECK_STR(conf,
              "rtx.ignoreTextures = -0x000000000000000D\n"
              "rtx.preTransformedVerticesIsUI = True\n"
              "rtx.skyBoxTextures = 0x000000000000000A, 0x000000000000000B\n"
              "rtx.skyMinZThreshold = 0.5\n"
              "rtx.uiTextures = 0x000000000000000C\n");
    const GameProfile imported = importRtxConf(conf);
    CHECK_STR(exportRtxConf(imported), conf); // byte-stable both ways
    CHECK(imported.textures.at("sky").positives() == (HashSet{0xA, 0xB}));
    CHECK(imported.options.count("rtx.skyBoxTextures") == 0);

    // Remix rtx.conf with [exe] sections, lower-case hashes and non-rtx keys.
    options::ConfigParseOptions parse;
    parse.exeName = "Game.exe";
    const GameProfile g = importRtxConf("# comment\n"
                                        "rtx.uiTextures = 0xabc, 0xdef\n"
                                        "d3d9.maxFrameRate = 60\n"
                                        "[Other.exe]\n"
                                        "rtx.skyBoxTextures = 0x1\n"
                                        "[Game.exe]\n"
                                        "rtx.decalTextures = 0x2, -0x3\n",
                                        parse);
    CHECK(g.textures.at("ui").positives() == (HashSet{0xABC, 0xDEF}));
    CHECK(g.textures.count("sky") == 0);
    CHECK(g.textures.at("decal").hasNegative(0x3));
    CHECK_STR(g.options.at("d3d9.maxFrameRate"), "60");
    // Profile JSON -> rtx.conf -> profile JSON keeps the categories.
    GameProfile again = importRtxConf(exportRtxConf(g));
    again.name = g.name;
    CHECK(again.textures.at("decal") == g.textures.at("decal"));
}

void testCategoriesAreRealOptions() {
    for (const TextureCategory& c : textureCategories()) {
        const options::OptionBase* o = options::OptionManager::findOption(c.optionKey);
        CHECK(o != nullptr);
        if (!o) {
            std::fprintf(stderr, "  missing option %s\n", c.optionKey);
        }
        CHECK(findTextureCategory(c.name) == &c);
        CHECK(findTextureCategory(c.optionKey) == &c);
    }
}

// ---- option layers --------------------------------------------------------------------------------------------

void testProfileLayerPriority() {
    using scene::ClassifyOptions;
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_setup_layers");
    GameProfile p;
    p.name = "layers";
    p.options["rtx.skyMinZThreshold"] = "0.25";
    p.options["rtx.skyDrawcallIdThreshold"] = "7";
    p.options["rtx.orthographicIsUI"] = "False";
    p.addTexture("sky", 0xA);
    p.addTexture("sky", 0xB);
    p.addTexture("ui", 0x10);

    // dxvk.conf (priority 1) < profile (2) < rtx.conf (3).
    writeFile(dir / "dxvk.conf", "rtx.skyDrawcallIdThreshold = 3\nrtx.skyMinZThreshold = 0.75\n");
    writeFile(dir / "rtx.conf", "rtx.skyMinZThreshold = 0.9\nrtx.skyBoxTextures = -0xA, 0xC\n");

    options::OptionSystemDesc desc;
    desc.baseDirectory = dir.string();
    desc.exeName = "Game.exe";
    desc.appConfig = profileToConfig(p);
    options::OptionSystem::initialize(desc);

    CHECK(ClassifyOptions::skyDrawcallIdThreshold() == 7u); // profile beats dxvk.conf
    CHECK(ClassifyOptions::skyMinZThreshold() == 0.9f);     // rtx.conf beats the profile
    CHECK(!ClassifyOptions::orthographicIsUI());            // profile beats the default
    CHECK((ClassifyOptions::skyBoxTextures() == HashSet{0xB, 0xC})); // rtx.conf `-0xA` removes a profile hash
    CHECK((ClassifyOptions::uiTextures() == HashSet{0x10}));

    // A profile loaded as a dynamic layer beats rtx.conf; releasing it restores the stack.
    GameProfile preview;
    preview.name = "preview";
    preview.options["rtx.skyMinZThreshold"] = "0.1";
    preview.addTexture("sky", 0xA);
    {
        options::OptionLayerHandle layer = applyProfileLayer(preview, options::kMinDynamicLayerPriority);
        apply();
        CHECK(ClassifyOptions::skyMinZThreshold() == 0.1f);
        CHECK((ClassifyOptions::skyBoxTextures() == HashSet{0xA, 0xB, 0xC}));
    }
    apply();
    CHECK(ClassifyOptions::skyMinZThreshold() == 0.9f);
    CHECK((ClassifyOptions::skyBoxTextures() == HashSet{0xB, 0xC}));

    // The environment layer beats the profile (DXVK_ENABLE_RAYTRACING backs rtx.enableRaytracing).
    options::OptionSystem::shutdown();
    p.options["rtx.enableRaytracing"] = "False";
    desc.appConfig = profileToConfig(p);
    options::setEnvironmentVariable("DXVK_ENABLE_RAYTRACING", "True");
    options::OptionSystem::initialize(desc);
    options::setEnvironmentVariable("DXVK_ENABLE_RAYTRACING", "");
    CHECK(ClassifyOptions::enableRaytracing());
    options::OptionSystem::shutdown();
    options::OptionSystem::initialize(desc);
    CHECK(!ClassifyOptions::enableRaytracing());
    options::OptionSystem::shutdown();
    CHECK(ClassifyOptions::orthographicIsUI()); // back to the default
}

// ---- discovery ------------------------------------------------------------------------------------------------

std::string profileText(const std::string& name, const std::string& exe, const std::string& hash, bool requireHash) {
    GameProfile p;
    p.name = name;
    if (!exe.empty()) {
        p.match.exeNames.push_back(exe);
    }
    std::uint64_t h = 0;
    if (!hash.empty() && parseHash(hash, h)) {
        p.match.exeHashes.push_back(h);
    }
    p.match.requireHash = requireHash;
    p.options["rtx.skyDrawcallIdThreshold"] = "1";
    return writeProfile(p);
}

void testDiscovery() {
    const std::filesystem::path dir = fuse::test::makeUniqueTempDir("rl_setup_discovery");
    const std::filesystem::path exe = dir / "bin" / "Game.EXE";
    writeFile(exe, "MZ fake executable bytes");
    const std::string bytes = "MZ fake executable bytes";
    const std::uint64_t exeHash = hash::xxh3_64(bytes.data(), bytes.size());
    CHECK(hashExecutableFile(exe.string()) == exeHash);
    CHECK(!hashExecutableFile((dir / "missing.exe").string()));

    const std::filesystem::path db = dir / "db";
    writeFile(db / "a_name.json", profileText("by-name", "game.exe", "", false));
    writeFile(db / "b_other.json", profileText("other", "Other.exe", "", false));
    writeFile(db / "c_strict.json", profileText("strict", "game.exe", "0x1234", true));
    writeFile(db / "d_broken.json", "{ not json");
    writeFile(db / "readme.txt", "ignored");

    DiscoveryRequest request;
    request.exe.path = exe.string();
    request.searchDirs = {db.string()};
    DiscoveryResult r = discoverProfile(request);
    CHECK(r.profile && r.profile->name == "by-name"); // case-insensitive name; strict one needs its hash
    CHECK(r.score == 1);

    // A hash match beats a name match, whatever the file order.
    writeFile(db / "e_hash.json", profileText("by-hash", "", formatHash(exeHash), true));
    r = discoverProfile(request);
    CHECK(r.profile && r.profile->name == "by-hash");
    CHECK(r.score == 2);

    // Preset identity (no file): name only.
    DiscoveryRequest named;
    named.exe.name = "GAME.exe";
    named.exe.hash = 0x42;
    named.searchDirs = {db.string()};
    r = discoverProfile(named);
    CHECK(r.profile && r.profile->name == "by-name");

    // Equal scores: the earlier directory, then the earlier file name.
    const std::filesystem::path db2 = dir / "db2";
    writeFile(db2 / "0_first.json", profileText("db2-name", "game.exe", "", false));
    named.searchDirs = {db2.string(), db.string()};
    r = discoverProfile(named);
    CHECK(r.profile && r.profile->name == "db2-name");

    // Explicit profile, no matching.
    DiscoveryRequest explicitRequest;
    explicitRequest.exe.name = "unrelated.exe";
    explicitRequest.explicitProfile = (db / "b_other.json").string();
    r = discoverProfile(explicitRequest);
    CHECK(r.profile && r.profile->name == "other" && r.score == 3);

    // Nothing matches.
    DiscoveryRequest none;
    none.exe.name = "nothing.exe";
    none.searchDirs = {db.string()};
    CHECK(!discoverProfile(none).profile);

    // Search path from the environment; default dirs otherwise.
    options::setEnvironmentVariable(kProfilePathEnvVar, db2.string() + ";" + db.string());
    const std::vector<std::string> dirs = defaultProfileSearchDirs(exe.string());
    CHECK(dirs.size() == 2 && dirs[0] == db2.string());
    options::setEnvironmentVariable(kProfilePathEnvVar, "");
    const std::vector<std::string> defaults = defaultProfileSearchDirs(exe.string(), dir.string());
    CHECK(!defaults.empty() && defaults[0].find("relight/profiles") != std::string::npos);

    // runtimeOptionSystemDesc: FUSE_RELIGHT_PROFILE = off disables; a path forces that profile.
    options::setEnvironmentVariable(kProfileEnvVar, "off");
    CHECK(runtimeOptionSystemDesc(dir.string()).appConfig.empty());
    options::setEnvironmentVariable(kProfileEnvVar, (db / "a_name.json").string());
    const options::OptionSystemDesc desc = runtimeOptionSystemDesc(dir.string());
    CHECK(desc.appConfig.get<std::uint32_t>("rtx.skyDrawcallIdThreshold", 0) == 1u);
    CHECK_STR(runtimeProfileName(), "by-name");
    options::setEnvironmentVariable(kProfileEnvVar, "");
}

// ---- assistant ------------------------------------------------------------------------------------------------

std::string drawLine(int frame, int di, const char* tex, const char* desc, int texId, const char* status,
                     const char* reason, const char* categories, const char* camera, bool zEnable, bool zWrite,
                     double minZ, bool blend, int dst, bool alphaTest, int vc, const char* key,
                     const char* aabb = "0,0,0,0,0,0") {
    char buf[1400];
    std::snprintf(buf, sizeof(buf),
                  R"({"ev":"draw","n":%d,"frame":%d,"di":%d,"geometry":{"status":"captured","vc":"%d","key":"%s","aabb":"%s"},)"
                  R"("textures":[%s],"classification":{"status":"%s","reason":"%s","categories":"%s","color_texture":"0x%s"},)"
                  R"("translation":{"camera":"%s","z_enable":%s,"z_write":%s,"min_z":%g,"viewport":[0,0,128,96],)"
                  R"("material":{"blend":%s,"color_dst":%d,"alpha_test":%s}}})",
                  frame * 100 + di, frame, di, vc, key, aabb,
                  tex[0] ? (std::string(R"({"slot":0,"texture":)") + std::to_string(texId) + R"(,"hash":")" + tex +
                            R"(","desc":")" + desc + R"("})").c_str()
                         : "",
                  status, reason, categories, tex[0] ? tex : "0", camera, zEnable ? "true" : "false",
                  zWrite ? "true" : "false", minZ, blend ? "true" : "false", dst, alphaTest ? "true" : "false");
    return buf;
}

std::string cameraLine(int frame, double fov) {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  R"({"ev":"translate_frame","frame":%d,"cameras":[{"type":"Main","fov":%g,"near":0.1,"far":100}]})",
                  frame, fov);
    return buf;
}

const ProfileSuggestion* find(const std::vector<ProfileSuggestion>& list, const char* category, std::uint64_t hash) {
    for (const ProfileSuggestion& s : list) {
        if (s.kind == ProfileSuggestion::Kind::Texture && s.category == category && s.hash == hash) {
            return &s;
        }
    }
    return nullptr;
}

const ProfileSuggestion* findOption(const std::vector<ProfileSuggestion>& list, const char* key) {
    for (const ProfileSuggestion& s : list) {
        if (s.kind == ProfileSuggestion::Kind::Option && s.key == key) {
            return &s;
        }
    }
    return nullptr;
}

// POSITIONT AABBs in pixels (hex floats): a small HUD block and a full-screen quad.
constexpr const char* kSmallAabb = "40800000,40800000,0,42300000,41300000,0";      // 4,4 .. 44,11
constexpr const char* kFullAabb = "0,0,0,43000000,42c00000,0";                      // 0,0 .. 128,96

void testAssistantSkyUiHud() {
    std::string jsonl = R"({"ev":"header","schema":"fuse.relight.capture/1"})" "\n";
    for (int f = 0; f < 3; ++f) {
        jsonl += drawLine(f, 0, "C0BD341CA38B60FC", "0000000000000000", 3, "raytraced", "RayTraced", "Sky", "Sky", false, false, 1, false, 7, false, 24, "4dea70187d2d1944") + "\n";
        jsonl += drawLine(f, 1, "306EECD4FA3B3B59", "0000000000000000", 4, "raytraced", "RayTraced", "", "Main", true, true, 0, false, 7, false, 81, "ccbb672126c7c777") + "\n";
        jsonl += drawLine(f, 2, "4EBAA29E7E1F48C5", "0000000000000000", 5, "raytraced", "RayTraced", "", "Main", true, true, 0, false, 7, false, 24, "c628a9fa6a1dc7f6") + "\n";
        jsonl += drawLine(f, 3, "", "", -1, "raytraced", "RayTraced", "", "Unknown", false, true, 0, true, 7, false, 4, "72c639b84f4dd05a") + "\n";
        jsonl += drawLine(f, 4, "4EBAA29E7E1F48C5", "0000000000000000", 5, "raytraced", "RayTraced", "", "Unknown", false, true, 0, true, 7, false, 4, "cef18c71c2a9a1b0") + "\n";
        jsonl += drawLine(f, 5, "", "", -1, "rasterized", "PositionT", "", "Unknown", false, true, 0, true, 7, false, 12, "8c033a932eaa5fca", kSmallAabb) + "\n";
        jsonl += cameraLine(f, 1.047) + "\n";
        jsonl += R"({"ev":"frame","frame":)" + std::to_string(f) + R"(,"draws":6})" "\n";
    }
    std::string error;
    const std::optional<SetupReport> report = analyzeCaptureRecord(jsonl, &error);
    CHECK(report.has_value());
    if (!report) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return;
    }
    CHECK(report->frames == 3 && report->draws == 18);
    const ProfileSuggestion* sky = find(report->proposals, "sky", 0xC0BD341CA38B60FCull);
    CHECK(sky && sky->confidence >= 0.9);
    const ProfileSuggestion* ui = find(report->proposals, "ui", 0x4EBAA29E7E1F48C5ull);
    CHECK(ui && ui->confidence < 0.8); // shared with world cubes: review only
    CHECK(ui && ui->reason.find("world") != std::string::npos);
    CHECK(!find(report->proposals, "ui", 0x306EECD4FA3B3B59ull));
    CHECK(!find(report->proposals, "sky", 0x306EECD4FA3B3B59ull));
    const ProfileSuggestion* pt = findOption(report->proposals, "rtx.preTransformedVerticesIsUI");
    CHECK(pt && pt->confidence >= 0.8);
    CHECK(report->cameraIssues.empty());
    CHECK(report->raytracedScreenSpace == 6);

    const GameProfile p = proposeProfile(*report, "sky_ui_hud", "sky_ui_hud.exe", 0x99ull);
    CHECK(p.textures.at("sky").hasPositive(0xC0BD341CA38B60FCull));
    CHECK(p.textures.count("ui") == 0);
    CHECK_STR(p.options.at("rtx.preTransformedVerticesIsUI"), "True");
    CHECK(p.match.exeNames == std::vector<std::string>{"sky_ui_hud.exe"});
    CHECK(p.match.exeHashes == std::vector<std::uint64_t>{0x99});
    CHECK_STR(writeProfile(*parseProfile(writeProfile(p))), writeProfile(p));
    // Lower threshold: the conflicting UI texture gets applied too.
    AssistantOptions lax;
    lax.applyThreshold = 0.3;
    const GameProfile q = proposeProfile(*report, "lax", "sky_ui_hud.exe", std::nullopt, lax);
    CHECK(q.textures.count("ui") == 1 && q.textures.at("ui").hasPositive(0x4EBAA29E7E1F48C5ull));
}

void testAssistantOtherRules() {
    std::string jsonl;
    for (int f = 0; f < 2; ++f) {
        // render target sampled by a world quad, and the offscreen pass itself (skipped)
        jsonl += drawLine(f, 0, "1F87022E00000000", "0000000000000000", 5, "rasterized", "NonPrimaryTarget", "", "Unknown", true, true, 0, false, 0, false, 24, "aaaa") + "\n";
        jsonl += drawLine(f, 1, "0D147D0000000000", "96A65D0000000001", 3, "raytraced", "RayTraced", "", "Main", true, true, 0, false, 0, false, 4, "bbbb") + "\n";
        // additive sprites: particles; alpha-blended overlay with z-writes off: decal
        jsonl += drawLine(f, 2, "5A5A5A5A5A5A5A5A", "0000000000000000", 6, "raytraced", "RayTraced", "", "Main", true, false, 0, true, 1, false, 16, "cccc") + "\n";
        jsonl += drawLine(f, 3, "DECA1DECA1DECA10", "0000000000000000", 7, "raytraced", "RayTraced", "", "Main", true, false, 0, true, 7, false, 300, "dddd") + "\n";
        // full-screen POSITIONT darkening quad at the end
        jsonl += drawLine(f, 9, "", "", -1, "rasterized", "PositionT", "", "Unknown", false, true, 0, true, 7, false, 4, "eeee", kFullAabb) + "\n";
        // dynamic geometry: same slot, new asset hash each frame
        jsonl += drawLine(f, 5, "0000000000000077", "0000000000000000", 8, "raytraced", "RayTraced", "", "Main", true, true, 0, false, 0, false, 30, f ? "f001" : "f000") + "\n";
        jsonl += cameraLine(f, f == 0 ? 3.1 : 1.0) + "\n";
    }
    jsonl += R"({"ev":"textures","frame":1,"textures":[{"id":3,"hash":"0D147D0000000000","desc":"96A65D0000000001","origin":"render_target"}]})" "\n";
    // a frame with 3D draws and no camera record at all
    jsonl += drawLine(2, 0, "", "", -1, "raytraced", "RayTraced", "", "Main", true, true, 0, false, 0, false, 3, "9999") + "\n";
    jsonl += R"({"ev":"translate_frame","frame":2,"cameras":[]})" "\n";

    std::string error;
    const std::optional<SetupReport> report = analyzeCaptureRecord(jsonl, &error);
    CHECK(report.has_value());
    if (!report) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return;
    }
    const ProfileSuggestion* rt = find(report->proposals, "raytracedRenderTarget", 0x96A65D0000000001ull);
    CHECK(rt && rt->confidence < 0.8);
    CHECK(!find(report->proposals, "raytracedRenderTarget", 0x1F87022E00000000ull));
    CHECK(find(report->proposals, "particle", 0x5A5A5A5A5A5A5A5Aull));
    CHECK(find(report->proposals, "decal", 0xDECA1DECA1DECA10ull));
    CHECK(!find(report->proposals, "decal", 0x5A5A5A5A5A5A5A5Aull));
    const ProfileSuggestion* pt = findOption(report->proposals, "rtx.preTransformedVerticesIsUI");
    CHECK(pt && pt->confidence < 0.8 && pt->reason.find("full-screen") != std::string::npos);
    const ProfileSuggestion* rule = findOption(report->proposals, "rtx.geometryAssetHashRuleString");
    CHECK(rule != nullptr);
    bool badFov = false, noMain = false;
    for (const CameraIssue& i : report->cameraIssues) {
        badFov = badFov || (i.frame == 0 && i.message.find("FOV") != std::string::npos);
        noMain = noMain || (i.frame == 2 && i.message.find("no main camera") != std::string::npos);
    }
    CHECK(badFov);
    CHECK(noMain);
    const GameProfile p = proposeProfile(*report, "other", "other.exe");
    CHECK(p.textures.empty()); // nothing reaches the default threshold here
    CHECK(p.options.empty());
    CHECK(!p.notes.empty());
    CHECK(!formatReport(*report).empty());

    CHECK(!analyzeCaptureRecord("{\"ev\":\"draw\"}\nnot json\n", &error));
    CHECK(error.find("line 2") != std::string::npos);
}

} // namespace

int main() {
    // Hermetic: path overrides from the caller's environment would change what loads.
    options::setEnvironmentVariable(options::kDxvkConfEnvVar, "");
    options::setEnvironmentVariable(options::kRtxConfEnvVar, "");
    options::setEnvironmentVariable(kProfileEnvVar, "");
    options::setEnvironmentVariable(kProfilePathEnvVar, "");

    struct Case {
        const char* name;
        void (*fn)();
    };
    const Case cases[] = {
        {"profile_round_trip", testProfileRoundTrip},
        {"accept_suggestions", testAcceptSuggestions},
        {"rtx_conf_round_trip", testRtxConfRoundTrip},
        {"categories_are_options", testCategoriesAreRealOptions},
        {"profile_layer_priority", testProfileLayerPriority},
        {"discovery", testDiscovery},
        {"assistant_sky_ui_hud", testAssistantSkyUiHud},
        {"assistant_other_rules", testAssistantOtherRules},
    };
    for (const Case& c : cases) {
        g_test = c.name;
        const int before = g_failures;
        c.fn();
        std::printf("[rl_setup] %s: %s\n", c.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("rl_setup: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
