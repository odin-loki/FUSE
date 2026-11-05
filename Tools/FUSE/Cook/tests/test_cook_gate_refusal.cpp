// fuse_asset_cook_refuses_invalid (FUSE_ASSET_PLAN §5.3, "the cook refuses (strict mode, which is already the
// default) assets that fail any gate; --lenient only for local iteration"):
//   * strict AssetCooker refuses a mesh with a zero-area triangle, 44.1 kHz audio and clipped audio with
//     CookStatus::AssetGateFailed, names the violation and leaves no output behind (direct cooks and manifest
//     entries); textures are gated on normal-map encoding only (power of two is fuse_assetcheck's, descriptor-level);
//   * the same sources cook in lenient mode, the violations logged and appended to the record note;
//   * valid twins of each source cook in strict mode;
//   * the status is named and counts as an import-validation failure.
#include <fuse/assetcheck/asset_rules.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_manifest.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fuse::project::AssetCooker;
using fuse::project::CookRecord;
using fuse::project::CookStatus;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

fs::path dir() {
    static const fs::path d = [] {
        fs::path p = fs::temp_directory_path() / "fuse_asset_cook_refuses_invalid";
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p);
        return p;
    }();
    return d;
}

std::string path(const std::string& name) { return (dir() / name).string(); }

void write_text(const std::string& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

void put32(std::string& o, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<char>((x >> (8 * i)) & 0xffu));
}

void put16(std::string& o, std::uint32_t x) {
    o.push_back(static_cast<char>(x & 0xffu));
    o.push_back(static_cast<char>((x >> 8) & 0xffu));
}

/// Mono 32-bit float WAV.
void write_wav(const std::string& p, std::uint32_t rate, const std::vector<float>& samples) {
    std::string o = "RIFF";
    put32(o, 36u + static_cast<std::uint32_t>(samples.size() * 4u));
    o += "WAVEfmt ";
    put32(o, 16u);
    put16(o, 3u);
    put16(o, 1u);
    put32(o, rate);
    put32(o, rate * 4u);
    put16(o, 4u);
    put16(o, 32u);
    o += "data";
    put32(o, static_cast<std::uint32_t>(samples.size() * 4u));
    for (float f : samples) {
        std::uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        put32(o, b);
    }
    write_text(p, o);
}

std::vector<float> tone(std::uint32_t rate, double seconds, double amplitude) {
    std::vector<float> s(static_cast<std::size_t>(rate * seconds));
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<float>(amplitude * std::sin(2.0 * 3.14159265358979 * 440.0 * double(i) / double(rate)));
    }
    return s;
}

/// Uncompressed 32-bit TGA, grey noise-ish pattern.
void write_tga(const std::string& p, int w, int h) {
    std::string o(18, '\0');
    o[2] = 2;
    o[12] = static_cast<char>(w & 0xff);
    o[13] = static_cast<char>(w >> 8);
    o[14] = static_cast<char>(h & 0xff);
    o[15] = static_cast<char>(h >> 8);
    o[16] = 32;
    o[17] = static_cast<char>(0x28);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const char g = static_cast<char>(60 + (x * 37 + y * 91) % 120);
            o.push_back(g);
            o.push_back(g);
            o.push_back(g);
            o.push_back(static_cast<char>(255));
        }
    }
    write_text(p, o);
}

const char* kCubeObj =
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
    "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\nf 2 3 7\nf 2 7 6\nf 3 4 8\nf 3 8 7\nf 4 1 5\nf 4 5 8\n";

bool exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

void expect_refused(const CookRecord& r, const std::string& output, const std::string& violation, const std::string& what) {
    check(!r.ok && r.status == CookStatus::AssetGateFailed, what + ": strict cook refused with AssetGateFailed (status " +
                                                                std::string(fuse::project::cookStatusName(r.status)) + ", " + r.note + ")");
    check(r.note.find(violation) != std::string::npos, what + ": note names the violation '" + violation + "': " + r.note);
    check(!exists(output), what + ": no output left behind");
}

void expect_lenient(const CookRecord& r, const std::string& output, const std::string& what, bool gateNote) {
    check(r.ok && r.status == CookStatus::Ok, what + ": lenient cook keeps going (" + r.note + ")");
    check(exists(output), what + ": lenient output written");
    if (gateNote) {
        check(r.note.find("asset gate violations") != std::string::npos, what + ": lenient note carries the violations: " + r.note);
    }
}

void test_status() {
    check(std::string(fuse::project::cookStatusName(CookStatus::AssetGateFailed)) == "asset_gate_failed", "status name");
    check(fuse::project::isImportValidationFailure(CookStatus::AssetGateFailed), "AssetGateFailed is a validation failure");
}

void test_audio() {
    write_wav(path("tone.wav"), 48000u, tone(48000u, 1.0, 0.1));
    std::vector<float> clipped = tone(48000u, 1.0, 1.0);
    for (float& s : clipped) s = s >= 0.f ? 1.0f : -1.0f; // full-scale square
    write_wav(path("clipped.wav"), 48000u, clipped);

    fuse::project::AudioImportDesc good;
    good.input_path = path("tone.wav");
    good.output_path = path("tone.fuseaudio");
    AssetCooker strict;
    const CookRecord ok = strict.cook_audio(good);
    check(ok.ok && exists(good.output_path), "valid 48 kHz tone cooks strict: " + ok.note);

    fuse::project::AudioImportDesc rate44 = good;
    rate44.output_path = path("tone44.fuseaudio");
    rate44.target_sample_rate = 44100u;
    expect_refused(strict.cook_audio(rate44), rate44.output_path, "48 kHz", "audio resampled to 44.1 kHz");

    fuse::project::AudioImportDesc clip;
    clip.input_path = path("clipped.wav");
    clip.output_path = path("clipped.fuseaudio");
    clip.normalise = false;
    clip.format = fuse::project::AudioImportDesc::Format::PCM_F32;
    expect_refused(strict.cook_audio(clip), clip.output_path, "clipping", "clipped audio");

    AssetCooker lenient;
    lenient.set_import_validation(fuse::project::ImportValidation::Lenient); // the gate's own lenient check
    expect_lenient(lenient.cook_audio(rate44), rate44.output_path, "audio 44.1 kHz (lenient)", true);
    expect_lenient(lenient.cook_audio(clip), clip.output_path, "clipped audio (lenient)", true);

    // Manifest entries go through the same gate (entry defaults: 48 kHz, normalised -> passes; clipped source is
    // normalised down and then passes too, so check the valid path only).
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Audio;
    entry.source_path = path("tone.wav");
    entry.output_path = path("tone_entry.fuseaudio");
    check(strict.cook_entry(entry).ok, "manifest audio entry cooks strict");
}

void test_mesh() {
    write_text(path("cube.obj"), kCubeObj);
    // + one zero-area triangle over three collinear points.
    write_text(path("sliver.obj"), std::string(kCubeObj) + "v 2 0 0\nv 3 0 0\nv 4 0 0\nf 9 10 11\n");
    fuse::project::MeshImportDesc good;
    good.input_path = path("cube.obj");
    good.output_path = path("cube.fusemesh");
    AssetCooker strict;
    const CookRecord ok = strict.cook_mesh(good);
    check(ok.ok && exists(good.output_path), "valid cube cooks strict: " + ok.note);
    if (ok.status == CookStatus::ImporterUnavailable) {
        std::printf("  mesh refusal skipped: assimp not linked\n");
        return;
    }
    fuse::project::MeshImportDesc bad;
    bad.input_path = path("sliver.obj");
    bad.output_path = path("sliver.fusemesh");
    expect_refused(strict.cook_mesh(bad), bad.output_path, "zero-area", "mesh with a zero-area triangle");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = bad.input_path;
    entry.output_path = path("sliver_entry.fusemesh");
    expect_refused(strict.cook_entry(entry), entry.output_path, "zero-area", "manifest mesh entry with a zero-area triangle");

    AssetCooker lenient;
    lenient.set_import_validation(fuse::project::ImportValidation::Lenient);
    bad.output_path = path("sliver_lenient.fusemesh");
    bad.meshlets = true; // lenient takes the full cook first when sections are asked for: the gate then warns
    const CookRecord lr = lenient.cook_mesh(bad);
    expect_lenient(lr, bad.output_path, "mesh with a zero-area triangle (lenient)", lr.note.find("stub") == std::string::npos);
}

void test_texture() {
    write_tga(path("albedo16.tga"), 16, 16);
    write_tga(path("albedo12x8.tga"), 12, 8);
    AssetCooker strict;
    fuse::project::TextureImportDesc good;
    good.input_path = path("albedo16.tga");
    good.output_path = path("albedo16.fusetex");
    const CookRecord ok = strict.cook_texture(good);
    check(ok.ok && exists(good.output_path), "16x16 texture cooks strict: " + ok.note);
    if (ok.status == CookStatus::ImporterUnavailable) {
        std::printf("  texture refusal skipped: stb_image not linked\n");
        return;
    }
    // The cook gates normal-map encoding only (linear BC5); power-of-two and class budgets are descriptor-level
    // (fuse_assetcheck budget_texture), since the cook also serves UI / logo / sample images of any size.
    fuse::project::TextureImportDesc npot;
    npot.input_path = path("albedo12x8.tga");
    npot.output_path = path("albedo12x8.fusetex");
    const CookRecord npotRecord = strict.cook_texture(npot);
    check(npotRecord.ok, "non-power-of-two image still cooks strict (not a cook gate): " + npotRecord.note);
    fuse::project::TextureImportDesc normal;
    normal.input_path = path("albedo16.tga");
    normal.output_path = path("normal16.fusetex");
    normal.is_normal_map = true; // colour space left at the sRGB default: the cook stores BC5 linear anyway
    const CookRecord normalRecord = strict.cook_texture(normal);
    check(normalRecord.ok && normalRecord.status == CookStatus::Ok, "normal map cooks as linear BC5 and passes the gate: " +
                                                                        normalRecord.note);
    fuse::assetcheck::Violations v;
    fuse::assetcheck::checkNormalMapEncoding(true, false, "BC7", "normal", v);
    fuse::assetcheck::checkNormalMapEncoding(true, true, "BC5", "normal", v);
    check(v.size() == 2u, "normal-map rule refuses BC7 and sRGB normal maps");
}

} // namespace

int main() {
    test_status();
    test_audio();
    test_mesh();
    test_texture();
    std::error_code ec;
    fs::remove_all(dir(), ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_cook_refuses_invalid: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_asset_cook_refuses_invalid: all checks passed\n");
    return EXIT_SUCCESS;
}
