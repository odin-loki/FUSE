// Appendix A rename row (master plan): "Scene file magic updated (versioned; old magic in compat
// loader)". SceneSerialiser writes the FUSE magic (bytes "FUSE"); the pre-rename 'ENGC' files
// still load through the compat path — both a synthesised one and the committed sample
// `.fuselevel` — and re-saving upgrades them to the FUSE magic with an otherwise identical body.
#include <fuse/scene/scene.hpp>
#include <fuse/scene/serialiser.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

using fuse::scene::SceneSerialiser;
using fuse::scene::SerialiseStatus;

std::vector<unsigned char> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeAll(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path tempDir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_scene_magic_gates";
    std::filesystem::create_directories(dir);
    return dir;
}

fuse::scene::Scene makeScene() {
    fuse::scene::Scene scene("MagicScene");
    scene.camera().setPosition(4.f, 5.f, 6.f);
    scene.camera().setOrientation(15.f, -10.f);
    fuse::scene::SceneEntityTransform t{};
    t.positionX = 1.5f;
    t.scaleY = 2.f;
    scene.addEntity("root", t);
    scene.addEntity("child", {}, 0);
    return scene;
}

void testNewMagicWritten() {
    expectTrue(SceneSerialiser::MAGIC != SceneSerialiser::LEGACY_MAGIC_ENGC, "magic changed from ENGC");
    const std::filesystem::path path = tempDir() / "fuse_magic.fuselevel";
    expectTrue(SceneSerialiser::save(makeScene(), path.string()).status == SerialiseStatus::Ok, "save ok");
    const std::vector<unsigned char> bytes = readAll(path);
    expectTrue(bytes.size() >= 64u && std::memcmp(bytes.data(), "FUSE", 4) == 0, "file starts with bytes FUSE");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult r = SceneSerialiser::load(path.string(), loaded);
    expectTrue(r.status == SerialiseStatus::Ok && !r.legacyMagic, "FUSE-magic file loads (not via compat)");
    expectTrue(loaded.entityCount() == 2u && loaded.entities()[1].parentIndex == 0 &&
                   loaded.entities()[0].transform.positionX == 1.5f,
               "FUSE-magic file content round-trips");
}

void testLegacyMagicCompat() {
    // Synthesised pre-rename file: identical layout, 'ENGC' magic (bytes "CGNE").
    const std::filesystem::path fusePath = tempDir() / "compat_src.fuselevel";
    const std::filesystem::path legacyPath = tempDir() / "compat_engc.fuselevel";
    const std::filesystem::path upgradedPath = tempDir() / "compat_upgraded.fuselevel";
    SceneSerialiser::save(makeScene(), fusePath.string());
    std::vector<unsigned char> legacy = readAll(fusePath);
    const fuse::u32 engc = SceneSerialiser::LEGACY_MAGIC_ENGC;
    std::memcpy(legacy.data(), &engc, sizeof(engc));
    expectTrue(std::memcmp(legacy.data(), "CGNE", 4) == 0, "legacy fixture carries ENGC magic bytes");
    writeAll(legacyPath, legacy);

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult r = SceneSerialiser::load(legacyPath.string(), loaded);
    expectTrue(r.status == SerialiseStatus::Ok, "ENGC file loads through the compat loader");
    expectTrue(r.legacyMagic, "compat load is reported (legacyMagic)");
    expectTrue(loaded.name() == "MagicScene" && loaded.entityCount() == 2u &&
                   loaded.entities()[0].transform.scaleY == 2.f && loaded.entities()[1].parentIndex == 0 &&
                   loaded.camera().positionZ == 6.f,
               "ENGC file content matches");

    expectTrue(SceneSerialiser::save(loaded, upgradedPath.string()).status == SerialiseStatus::Ok, "re-save ok");
    const std::vector<unsigned char> upgraded = readAll(upgradedPath);
    const std::vector<unsigned char> original = readAll(fusePath);
    expectTrue(upgraded == original, "re-saving an ENGC file upgrades it: FUSE magic, body byte-identical");
}

void testCommittedSampleStillLoads() {
    const std::filesystem::path sample =
        std::filesystem::path(FUSE_SOURCE_DIR) / "Samples/unification/demo_3d_empty/worlds/example.fuselevel";
    const std::vector<unsigned char> bytes = readAll(sample);
    expectTrue(bytes.size() >= 4u, "committed sample present");
    if (bytes.size() >= 4u && std::memcmp(bytes.data(), "CGNE", 4) == 0) {
        fuse::scene::Scene scene;
        const fuse::scene::SerialiseResult r = SceneSerialiser::load(sample.string(), scene);
        expectTrue(r.status == SerialiseStatus::Ok && r.legacyMagic, "committed ENGC sample loads via compat");
        std::printf("scene magic: committed sample (ENGC) loaded via compat, %u entities\n", scene.entityCount());
    } else {
        std::printf("scene magic: committed sample already carries the FUSE magic\n");
        expectTrue(bytes.size() >= 4u && std::memcmp(bytes.data(), "FUSE", 4) == 0, "sample magic is FUSE or ENGC");
    }
}

void testUnknownMagicRejected() {
    const std::filesystem::path src = tempDir() / "bad_src.fuselevel";
    const std::filesystem::path bad = tempDir() / "bad_magic.fuselevel";
    SceneSerialiser::save(makeScene(), src.string());
    std::vector<unsigned char> bytes = readAll(src);
    std::memcpy(bytes.data(), "TORQ", 4);
    writeAll(bad, bytes);
    fuse::scene::Scene scene;
    expectTrue(SceneSerialiser::load(bad.string(), scene).status == SerialiseStatus::InvalidMagic,
               "any other magic is still rejected");
}

} // namespace

int main() {
    testNewMagicWritten();
    testLegacyMagicCompat();
    testCommittedSampleStillLoads();
    testUnknownMagicRejected();
    if (g_failures == 0) {
        std::printf("fuse_scene_magic_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_scene_magic_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
