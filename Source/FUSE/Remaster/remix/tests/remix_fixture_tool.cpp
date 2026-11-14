// FUSE Remaster W2.4 test helper (remaster_remix gate):
//
//   remaster_remix_fixture author <project>
//       Writes our own replacement assets into the project store (synthetic, FUSE-generated, CC0-equivalent
//       LicenseRef-FUSE-Generated): mesh lib/fixture/crate (a 0.5 m cube with normals and UVs, metres),
//       texture_set lib/fixture/crate_tex (16x16 albedo + 8x8 normal map as TGA blobs), material lib/fixture/crate_mat,
//       and a recipe-only derivative der/fixture/upscaled (origin derived from an original oaid) for the
//       recipe-only refusal. Prints nothing; exit 0.
//   remaster_remix_fixture lint-negative <project> <mod dir> <scratch>
//       Copies the mod to <scratch>, then seeds (a) a blob of a captured original record, (b) a layer asset path
//       out of the mod folder, (c) a stray file, and expects checkExport to report each one. Exit 0 when it does.
#include <fuse/remaster/poco.hpp>
#include <fuse/remaster/remix/remix.hpp>
#include <fuse/remaster/store.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace rm = fuse::remaster;

namespace {

std::vector<std::uint8_t> tga(std::uint32_t w, std::uint32_t h, auto&& pixel) {
    std::vector<std::uint8_t> out(18, 0);
    out[2] = 2; // uncompressed true colour
    out[12] = static_cast<std::uint8_t>(w & 0xff);
    out[13] = static_cast<std::uint8_t>(w >> 8);
    out[14] = static_cast<std::uint8_t>(h & 0xff);
    out[15] = static_cast<std::uint8_t>(h >> 8);
    out[16] = 32;
    out[17] = 0x28; // top-left origin, 8 alpha bits
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto [r, g, b, a] = pixel(x, y);
            out.insert(out.end(), {b, g, r, a});
        }
    }
    return out;
}

template <class T>
std::vector<std::uint8_t> bytesOf(const std::vector<T>& v) {
    std::vector<std::uint8_t> out(v.size() * sizeof(T));
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}

rm::poco::Record header(rm::poco::Kind kind, const std::string& id) {
    rm::poco::Record r;
    r.kind = kind;
    r.id = id;
    r.name = id.substr(id.rfind('/') + 1);
    r.payload = rm::poco::defaultPayload(kind);
    r.provenance.origin = rm::poco::Origin::Generated;
    r.provenance.tool = "Source/FUSE/Remaster/remix/tests/remix_fixture_tool.cpp";
    r.provenance.human_authorship = rm::poco::HumanAuthorship::Substantial;
    r.licence_id = "LicenseRef-FUSE-Generated";
    r.distribution = rm::poco::Distribution::Ship;
    r.review.state = rm::poco::ReviewState::Approved;
    r.review.by = "fixture";
    r.review.date = "2026-11-05";
    return r;
}

bool author(const fs::path& project) {
    rm::store::BlobStore store(project / "store");
    std::string err;
    auto put = [&](const std::vector<std::uint8_t>& bytes, const char* media) {
        auto ref = store.put(bytes, media, &err);
        if (!ref) {
            std::fprintf(stderr, "put: %s\n", err.c_str());
            std::exit(1);
        }
        return *ref;
    };
    // A cube of half extent `h`, 24 vertices (flat normals), 12 triangles.
    auto cube = [&](float h, const std::string& id, bool derived, const std::string& oaid) {
        std::vector<float> pos, nrm, uv;
        std::vector<std::uint32_t> idx;
        const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (int f = 0; f < 6; ++f) {
            const float* N = n[f];
            const float U[3] = {N[1] != 0 || N[2] != 0 ? 1.f : 0.f, N[0] != 0 ? 1.f : 0.f, 0.f};
            const float V[3] = {N[1] * U[2] - N[2] * U[1], N[2] * U[0] - N[0] * U[2], N[0] * U[1] - N[1] * U[0]};
            const std::uint32_t base = static_cast<std::uint32_t>(pos.size() / 3);
            const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto& q : c) {
                for (int k = 0; k < 3; ++k) {
                    pos.push_back(h * (N[k] + q[0] * U[k] + q[1] * V[k]));
                    nrm.push_back(N[k]);
                }
                uv.push_back((q[0] + 1) / 2);
                uv.push_back((q[1] + 1) / 2);
            }
            idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        rm::poco::Record r = header(rm::poco::Kind::Mesh, id);
        auto& m = std::get<rm::poco::Mesh>(r.payload);
        m.streams.push_back({rm::poco::Semantic::Position, rm::poco::Format::F32x3, put(bytesOf(pos), "fuse/mesh-stream")});
        m.streams.push_back({rm::poco::Semantic::Normal, rm::poco::Format::F32x3, put(bytesOf(nrm), "fuse/mesh-stream")});
        m.streams.push_back({rm::poco::Semantic::Uv0, rm::poco::Format::F32x2, put(bytesOf(uv), "fuse/mesh-stream")});
        m.indices32 = put(bytesOf(idx), "fuse/mesh-indices");
        m.submeshes.push_back({0, static_cast<std::uint32_t>(idx.size()), "crate"});
        m.bounds = {-h, -h, -h, h, h, h};
        if (derived) {
            r.provenance.origin = rm::poco::Origin::Derived;
            r.provenance.derived_from = {"oaid:" + oaid};
            r.provenance.recipe = "recipe:mesh-upgrade/fixture@1";
            r.licence_id = "LicenseRef-FUSE-Generated"; // derived from an original oaid: computed recipe-only
            r.distribution = rm::poco::Distribution::RecipeOnly;
        }
        if (!store.writeRecord(r, &err)) {
            std::fprintf(stderr, "record %s: %s\n", id.c_str(), err.c_str());
            std::exit(1);
        }
    };
    cube(0.5f, "lib/fixture/crate", false, "");
    cube(0.25f, "der/fixture/upscaled", true, "00000000-0000-5000-8000-000000000000");

    rm::poco::Record ts = header(rm::poco::Kind::TextureSet, "lib/fixture/crate_tex");
    auto& set = std::get<rm::poco::TextureSet>(ts.payload);
    set.albedo = put(tga(16, 16, [](std::uint32_t x, std::uint32_t y) {
                         const bool on = ((x / 4) + (y / 4)) % 2 == 0;
                         return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(on ? 200 : 90), static_cast<std::uint8_t>(on ? 140 : 60),
                                                            static_cast<std::uint8_t>(on ? 80 : 30), 255};
                     }),
                     "image/x-tga");
    set.normal = put(tga(8, 8, [](std::uint32_t x, std::uint32_t) {
                         return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(118 + x * 3), 128, 255, 255};
                     }),
                     "image/x-tga");
    set.texelsPerMetre = 32.f;
    rm::poco::Record mat = header(rm::poco::Kind::Material, "lib/fixture/crate_mat");
    auto& m = std::get<rm::poco::Material>(mat.payload);
    m.textureSetId = "lib/fixture/crate_tex";
    m.roughness = 0.7f;
    m.metallic = 0.f;
    if (!store.writeRecord(ts, &err) || !store.writeRecord(mat, &err)) {
        std::fprintf(stderr, "record: %s\n", err.c_str());
        return false;
    }
    return true;
}

bool lintNegative(const fs::path& project, const fs::path& mod, const fs::path& scratch) {
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    fs::copy(mod, scratch, fs::copy_options::recursive, ec);
    if (ec) {
        std::fprintf(stderr, "copy: %s\n", ec.message().c_str());
        return false;
    }
    if (!rm::remix::checkExport(scratch, project).ok()) {
        std::fprintf(stderr, "the unmodified copy does not pass the check\n");
        return false;
    }
    // (a) a blob of a captured original record (the first one with a blob).
    rm::store::BlobStore store(project / "store");
    std::string stolen;
    for (const std::string& rel : store.listRecords()) {
        const auto text = rm::store::readFileText(store.root() / rel);
        const auto rec = text ? rm::poco::fromJsonText(*text) : std::nullopt;
        if (rec && rec->provenance.origin == rm::poco::Origin::Original) {
            if (const auto* mesh = std::get_if<rm::poco::Mesh>(&rec->payload); mesh && !mesh->streams.empty()) {
                stolen = mesh->streams[0].data.sha256;
                break;
            }
        }
    }
    const auto bytes = stolen.empty() ? std::nullopt : store.get(stolen);
    if (!bytes) {
        std::fprintf(stderr, "no original blob found in the store\n");
        return false;
    }
    rm::store::writeFileAtomic(scratch / "assets/meshes/stolen.bin", *bytes);
    // (b) an asset path out of the mod folder.
    rm::store::writeFileAtomic(scratch / "assets/meshes/escape.usda",
                               std::string_view("#usda 1.0\ndef Xform \"x\" (\n    prepend references = @../../../capture/mod.usda@\n)\n{\n}\n"));
    const rm::remix::CheckReport c = rm::remix::checkExport(scratch, project);
    auto has = [&](const char* needle) {
        for (const auto& e : c.errors) {
            if (e.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    for (const auto& e : c.errors) {
        std::printf("  seeded: %s\n", e.c_str());
    }
    const bool ok = has("stolen.bin is a blob of an original") && has("escape.usda references '../../../capture/mod.usda' outside") &&
                    has("stolen.bin is not part of the export");
    fs::remove_all(scratch, ec);
    std::printf("lint-negative: %s\n", ok ? "every seeded violation found" : "MISSED a seeded violation");
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "author") {
        return author(argv[2]) ? 0 : 1;
    }
    if (argc == 5 && std::string(argv[1]) == "lint-negative") {
        return lintNegative(argv[2], argv[3], argv[4]) ? 0 : 1;
    }
    std::fprintf(stderr, "usage: remaster_remix_fixture author <project> | lint-negative <project> <mod> <scratch>\n");
    return 2;
}
