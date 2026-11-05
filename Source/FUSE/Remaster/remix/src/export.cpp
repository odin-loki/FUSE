// FUSE Remaster W2.4: export-remix and its check (see remix.hpp).
#include "project.hpp"

#include <fuse/remaster/digest.hpp>
#include <fuse/remaster/poco.hpp>
#include <fuse/remaster/remix/remix.hpp>

#include <fuse/cook/bcn_encoder.hpp>
#include <fuse/relight/mods/assets/dds.hpp>
#include <fuse/relight/mods/import/mod_importer.hpp>
#include <fuse/relight/mods/usd/remix_profile.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
namespace rimp = fuse::relight::mods::import;
namespace rusd = fuse::relight::mods::usd;
namespace rast = fuse::relight::mods::assets;

namespace fuse::remaster::remix {
namespace {

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

std::string usdName(const std::string& id) {
    std::string out;
    for (char c : id) {
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(0, "a_");
    }
    return out;
}

std::string quoteUsd(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out + "\"";
}

template <class T>
std::vector<T> blobAs(const std::vector<std::uint8_t>& bytes) {
    std::vector<T> out(bytes.size() / sizeof(T));
    if (!out.empty()) {
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(T));
    }
    return out;
}

const poco::MeshStream* stream(const poco::Mesh& m, poco::Semantic s) {
    for (const auto& st : m.streams) {
        if (st.semantic == s) {
            return &st;
        }
    }
    return nullptr;
}

bool isNotShippable(const poco::Record& r) {
    return r.distribution != poco::Distribution::Ship || poco::computeDistribution(r) != poco::Distribution::Ship;
}
bool isOriginalRecord(const poco::Record& r) {
    return r.provenance.origin == poco::Origin::Original || r.distribution == poco::Distribution::Never ||
           poco::computeDistribution(r) == poco::Distribution::Never;
}

/// Every BlobRef sha256 in a record's payload.
void collectBlobs(const poco::Record& r, std::set<std::string>& out) {
    const json::Value p = poco::toJson(r)["payload"];
    std::vector<const json::Value*> stack{&p};
    while (!stack.empty()) {
        const json::Value* v = stack.back();
        stack.pop_back();
        if (v->isObject()) {
            const json::Value* sha = v->get("sha256");
            if (sha && v->get("media")) {
                out.insert(sha->asString());
            }
            for (const auto& [k, m] : v->asObject()) {
                stack.push_back(&m);
            }
        } else if (v->isArray()) {
            for (const auto& m : v->asArray()) {
                stack.push_back(&m);
            }
        }
    }
}

// ---- DDS (DX10 header, BC7 / BC5, full mip chain) ----------------------------------------------------------

store::ImageRGBA8 halve(const store::ImageRGBA8& src) {
    store::ImageRGBA8 dst;
    dst.width = std::max(1u, src.width / 2u);
    dst.height = std::max(1u, src.height / 2u);
    dst.pixels.resize(std::size_t(dst.width) * dst.height * 4u);
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                std::uint32_t sum = 0, n = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        const std::uint32_t sx = std::min(src.width - 1, x * 2 + dx), sy = std::min(src.height - 1, y * 2 + dy);
                        sum += src.pixels[(std::size_t(sy) * src.width + sx) * 4 + c];
                        ++n;
                    }
                }
                dst.pixels[(std::size_t(y) * dst.width + x) * 4 + c] = static_cast<std::uint8_t>((sum + n / 2) / n);
            }
        }
    }
    return dst;
}

std::vector<std::uint8_t> encodeDds(const store::ImageRGBA8& image, bool normalMap, bool srgb, std::string* error) {
    rast::TextureImage tex;
    tex.format = normalMap ? rast::TexFormat::BC5_UNORM_BLOCK : (srgb ? rast::TexFormat::BC7_SRGB_BLOCK : rast::TexFormat::BC7_UNORM_BLOCK);
    tex.width = image.width;
    tex.height = image.height;
    std::uint32_t levels = 1;
    for (std::uint32_t w = image.width, h = image.height; w > 1 || h > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
        ++levels;
    }
    tex.mipLevels = levels;
    if (rast::layoutSubresources(tex) == 0) {
        *error = "texture layout failed";
        return {};
    }
    store::ImageRGBA8 level = image;
    for (std::uint32_t mip = 0; mip < levels; ++mip) {
        cook::BcSourceImage src;
        src.width = level.width;
        src.height = level.height;
        src.rgba8 = level.pixels;
        std::vector<std::uint8_t> blocks;
        if (!cook::encode_bc_image(normalMap ? cook::BcFormat::BC5 : cook::BcFormat::BC7, src, blocks, error)) {
            return {};
        }
        tex.data.insert(tex.data.end(), blocks.begin(), blocks.end());
        if (mip + 1 < levels) {
            level = halve(level);
        }
    }
    return rast::writeDds(tex, error);
}

// ---- USD text ------------------------------------------------------------------------------------------------

std::string meshLayer(const std::string& name, const poco::Record& rec, const std::vector<float>& pos,
                      const std::vector<float>& nrm, const std::vector<float>& uv, const std::vector<std::uint32_t>& idx) {
    std::ostringstream o;
    o << "#usda 1.0\n(\n    defaultPrim = " << quoteUsd(name) << "\n    metersPerUnit = " << num(rec.units.metersPerUnit)
      << "\n    upAxis = \"Y\"\n)\n\n";
    o << "# FUSE Remaster export-remix: POCO record " << rec.id << " (" << rec.licence_id << ")\n";
    o << "def Xform " << quoteUsd(name) << "\n{\n    def Mesh \"mesh\"\n    {\n";
    o << "        int[] faceVertexCounts = [";
    for (std::size_t i = 0; i < idx.size() / 3; ++i) {
        o << (i ? ", " : "") << 3;
    }
    o << "]\n        int[] faceVertexIndices = [";
    for (std::size_t i = 0; i < idx.size(); ++i) {
        o << (i ? ", " : "") << idx[i];
    }
    o << "]\n";
    auto vecArray = [&](const char* decl, const std::vector<float>& v, std::size_t n, bool vertex) {
        o << "        " << decl << " = [";
        for (std::size_t i = 0; i + n <= v.size(); i += n) {
            o << (i ? ", " : "") << "(";
            for (std::size_t c = 0; c < n; ++c) {
                o << (c ? ", " : "") << num(v[i + c]);
            }
            o << ")";
        }
        o << "]" << (vertex ? " (\n            interpolation = \"vertex\"\n        )" : "") << "\n";
    };
    if (!nrm.empty()) {
        vecArray("normal3f[] normals", nrm, 3, true);
    }
    vecArray("point3f[] points", pos, 3, false);
    if (!uv.empty()) {
        vecArray("texCoord2f[] primvars:st", uv, 2, true);
    }
    o << "        uniform token subdivisionScheme = \"none\"\n    }\n}\n";
    return o.str();
}

} // namespace

ExportReport exportRemix(const ExportOptions& options) {
    ExportReport rep;
    std::string err;
    auto project = detail::loadProject(options.projectDir, &err);
    if (!project) {
        rep.errors.push_back(err);
        return rep;
    }
    const store::BlobStore blobs(project->store);
    double gameMpu = 1.0;
    if (const json::Value* r = project->doc.get("remix"); r && r->get("meters_per_unit") && r->get("meters_per_unit")->asDouble() > 0) {
        gameMpu = r->get("meters_per_unit")->asDouble();
    }

    struct MeshOut {
        ExportedReplacement r;
        std::string usdName;
        double scale = 1.0;
        std::array<double, 3> translate{0, 0, 0};
    };
    std::vector<MeshOut> meshes;
    std::vector<ExportedReplacement> materials;
    std::map<std::string, std::string> materialText; // prim name -> Material block
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::map<std::string, std::string> distributionOf; // oaid -> "ship" | "local-only"

    for (const db::Replacement& row : project->database->replacements()) {
        if (row.tier != "remix" || row.variant != "default") {
            continue;
        }
        const std::string where = "replacement " + row.oaid + " -> " + row.kind + "/" + row.pocoId;
        std::string algo, value;
        for (const db::HashKey& k : project->database->keysOf(row.oaid)) {
            if (k.algo == "remix.geom.asset" || k.algo == "remix.tex") {
                algo = k.algo;
                value = k.value;
            }
        }
        if (algo.empty()) {
            rep.errors.push_back(where + ": the original has no remix.geom.asset / remix.tex key (not from a Remix capture)");
            continue;
        }
        const auto rec = blobs.readRecord(row.kind, row.pocoId, &err);
        if (!rec) {
            rep.errors.push_back(where + ": " + err);
            continue;
        }
        if (isOriginalRecord(*rec)) {
            rep.errors.push_back(where + ": the record is an original / never distributed: it cannot go into a mod layer");
            continue;
        }
        if (isNotShippable(*rec) && !options.local) {
            rep.errors.push_back(where + ": the record is recipe-only (a derivative of an original): ship its recipe, or export "
                                         "a local mod with --local");
            continue;
        }
        distributionOf[row.oaid] = isNotShippable(*rec) ? "local-only" : "ship";
        ExportedReplacement out;
        out.oaid = row.oaid;
        out.remixHash = value;
        out.pocoId = row.pocoId;
        out.pocoKind = row.kind;
        const std::string name = usdName(row.pocoId);

        if (algo == "remix.geom.asset") {
            const auto* mesh = std::get_if<poco::Mesh>(&rec->payload);
            if (!mesh) {
                rep.errors.push_back(where + ": a mesh replacement needs a mesh record");
                continue;
            }
            const poco::MeshStream* p = stream(*mesh, poco::Semantic::Position);
            const poco::MeshStream* n = stream(*mesh, poco::Semantic::Normal);
            const poco::MeshStream* t = stream(*mesh, poco::Semantic::Uv0);
            auto load = [&](const poco::BlobRef& ref) {
                auto b = blobs.verify(ref, &err) ? blobs.get(ref.sha256) : std::nullopt;
                return b ? *b : std::vector<std::uint8_t>{};
            };
            if (!p || p->format != poco::Format::F32x3) {
                rep.errors.push_back(where + ": mesh without an F32x3 Position stream");
                continue;
            }
            const auto pos = blobAs<float>(load(p->data));
            const auto nrm = n && n->format == poco::Format::F32x3 ? blobAs<float>(load(n->data)) : std::vector<float>{};
            const auto uv = t && t->format == poco::Format::F32x2 ? blobAs<float>(load(t->data)) : std::vector<float>{};
            const auto idx = blobAs<std::uint32_t>(load(mesh->indices32));
            const std::size_t vc = pos.size() / 3;
            bool okIdx = !idx.empty() && idx.size() % 3 == 0 &&
                         std::all_of(idx.begin(), idx.end(), [&](std::uint32_t i) { return i < vc; });
            if (vc == 0 || !okIdx || (!nrm.empty() && nrm.size() != pos.size()) || (!uv.empty() && uv.size() / 2 != vc)) {
                rep.errors.push_back(where + ": mesh streams / indices are inconsistent");
                continue;
            }
            out.kind = "mesh";
            out.prim = "/RootNode/meshes/mesh_" + value;
            out.meshLayer = "assets/meshes/" + name + ".usda";
            const std::string text = meshLayer(name, *rec, pos, nrm, uv, idx);
            files[out.meshLayer] = std::vector<std::uint8_t>(text.begin(), text.end());
            MeshOut mo{out, name, rec->units.metersPerUnit / gameMpu, {0, 0, 0}};
            if (row.fit.isObject()) { // §2.7 fit block: uniform scale and a translation in metres
                if (const json::Value* s = row.fit.get("scale"); s && s->isNumber()) {
                    mo.scale *= s->asDouble();
                }
                if (const json::Value* tr = row.fit.get("translate"); tr && tr->asArray().size() == 3) {
                    for (int i = 0; i < 3; ++i) {
                        mo.translate[i] = tr->asArray()[i].asDouble() / gameMpu;
                    }
                }
            }
            meshes.push_back(std::move(mo));
        } else {
            const auto* mat = std::get_if<poco::Material>(&rec->payload);
            if (!mat) {
                rep.errors.push_back(where + ": a material replacement (remix.tex key) needs a material record");
                continue;
            }
            out.kind = "material";
            out.prim = "/RootNode/Looks/mat_" + value;
            std::ostringstream m;
            const std::string prim = "mat_" + value;
            m << "        def Material \"" << prim << "\"\n        {\n"
              << "            token outputs:mdl:surface.connect = </RootNode/Looks/" << prim << "/Shader.outputs:out>\n\n"
              << "            def Shader \"Shader\"\n            {\n"
              << "                uniform token info:implementationSource = \"sourceAsset\"\n"
              << "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
              << "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n"
              << "                color3f inputs:diffuse_color_constant = (" << num(mat->baseColor[0]) << ", " << num(mat->baseColor[1])
              << ", " << num(mat->baseColor[2]) << ")\n"
              << "                float inputs:reflection_roughness_constant = " << num(mat->roughness) << "\n"
              << "                float inputs:metallic_constant = " << num(mat->metallic) << "\n";
            if (!mat->textureSetId.empty()) {
                const auto ts = blobs.readRecord("texture_set", mat->textureSetId, &err);
                const auto* set = ts ? std::get_if<poco::TextureSet>(&ts->payload) : nullptr;
                if (!set || isOriginalRecord(*ts) || (isNotShippable(*ts) && !options.local)) {
                    rep.errors.push_back(where + ": texture set '" + mat->textureSetId + "' is missing, an original or not shippable");
                    continue;
                }
                const std::string tsName = usdName(mat->textureSetId);
                struct Slot {
                    const char* slot;
                    const std::optional<poco::BlobRef>* ref;
                    const char* input;
                    const char* colorSpace;
                    bool normal, srgb;
                };
                const Slot slots[] = {{"albedo", &set->albedo, "diffuse_texture", "auto", false, set->albedoSrgb},
                                      {"normal", &set->normal, "normalmap_texture", "raw", true, false},
                                      {"emissive", &set->emissive, "emissive_mask_texture", "auto", false, true},
                                      {"height", &set->height, "height_texture", "raw", false, false}};
                bool bad = false;
                for (const Slot& s : slots) {
                    if (!*s.ref) {
                        continue;
                    }
                    const auto bytes = blobs.verify(**s.ref, &err) ? blobs.get((*s.ref)->sha256) : std::nullopt;
                    auto image = bytes ? store::decodeImage(*bytes, &err) : std::nullopt;
                    if (image && s.normal && set->normalConvention == poco::NormalConvention::DX) {
                        // Remix normal maps are OpenGL (+Y) convention (remix-format-notes.md §5): flip green.
                        for (std::size_t px = 1; px < image->pixels.size(); px += 4) {
                            image->pixels[px] = static_cast<std::uint8_t>(255 - image->pixels[px]);
                        }
                    }
                    const auto dds = image ? encodeDds(*image, s.normal, s.srgb, &err) : std::vector<std::uint8_t>{};
                    if (dds.empty()) {
                        rep.errors.push_back(where + ": " + s.slot + " texture: " + err);
                        bad = true;
                        break;
                    }
                    const std::string rel = "assets/textures/" + tsName + "_" + s.slot + ".dds";
                    files[rel] = dds;
                    out.textures[s.slot] = rel;
                    m << "                asset inputs:" << s.input << " = @../" << rel << "@ (\n                    colorSpace = \""
                      << s.colorSpace << "\"\n                )\n";
                }
                if (bad) {
                    continue;
                }
            }
            m << "                token outputs:out (\n                    renderType = \"material\"\n                )\n"
              << "            }\n        }\n";
            materialText[prim] = m.str();
            materials.push_back(out);
        }
    }
    if (!rep.errors.empty()) {
        return rep;
    }
    if (meshes.empty() && materials.empty()) {
        rep.errors.push_back("no replacement rows with tier \"remix\" (add them with `fuse_remaster replace`)");
        return rep;
    }
    std::sort(meshes.begin(), meshes.end(), [](const MeshOut& a, const MeshOut& b) { return a.r.prim < b.r.prim; });

    // mod.usda
    std::ostringstream mod;
    mod << "#usda 1.0\n(\n    customLayerData = {\n        string lightspeed_game_name = " << quoteUsd(project->gameId)
        << "\n        string lightspeed_layer_type = \"replacement\"\n    }\n    defaultPrim = \"RootNode\"\n    metersPerUnit = "
        << num(gameMpu) << "\n";
    if (!materials.empty()) {
        mod << "    subLayers = [\n        @./materials/materials.usda@\n    ]\n";
    }
    mod << "    upAxis = \"Y\"\n)\n\n# FUSE Remaster export-remix (" << options.modName
        << "): replacements of the remaster project; our assets only (docs/remaster/remix-format-notes.md section 4).\n";
    mod << "over \"RootNode\"\n{\n";
    if (!meshes.empty()) {
        mod << "    over \"meshes\"\n    {\n";
        for (const MeshOut& m : meshes) {
            mod << "        over \"mesh_" << m.r.remixHash << "\"\n        {\n"
                << "            int preserveOriginalDrawCall = 0\n\n"
                << "            def Xform \"ref_" << m.usdName << "\" (\n                prepend references = @./" << m.r.meshLayer
                << "@\n            )\n            {\n"
                << "                double3 xformOp:scale = (" << num(m.scale) << ", " << num(m.scale) << ", " << num(m.scale) << ")\n"
                << "                double3 xformOp:translate = (" << num(m.translate[0]) << ", " << num(m.translate[1]) << ", "
                << num(m.translate[2]) << ")\n"
                << "                uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:scale\"]\n"
                << "            }\n        }\n";
        }
        mod << "    }\n";
    }
    mod << "}\n";
    const std::string modText = mod.str();
    files["mod.usda"] = std::vector<std::uint8_t>(modText.begin(), modText.end());
    if (!materials.empty()) {
        std::string t = "#usda 1.0\n\ndef Xform \"RootNode\"\n{\n    def Scope \"Looks\"\n    {\n";
        bool first = true;
        for (const auto& [prim, block] : materialText) {
            t += (first ? "" : "\n") + block;
            first = false;
        }
        t += "    }\n}\n";
        files["materials/materials.usda"] = std::vector<std::uint8_t>(t.begin(), t.end());
    }

    // Manifest.
    std::vector<ExportedReplacement> all;
    for (const MeshOut& m : meshes) {
        all.push_back(m.r);
    }
    all.insert(all.end(), materials.begin(), materials.end());
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.prim < b.prim; });
    json::Value man = json::Value::object();
    man["schema"] = kExportSchema;
    man["mod"] = "mod.usda";
    man["mod_name"] = options.modName;
    man["game_id"] = project->gameId;
    man["meters_per_unit"] = gameMpu;
    json::Value reps = json::Value::array();
    for (const ExportedReplacement& r : all) {
        json::Value o = json::Value::object();
        o["oaid"] = r.oaid;
        o["kind"] = r.kind;
        o["remix_hash"] = r.remixHash;
        o["prim"] = r.prim;
        o["poco"] = r.pocoKind + "/" + r.pocoId;
        o["distribution"] = distributionOf[r.oaid];
        if (!r.meshLayer.empty()) {
            o["mesh_layer"] = r.meshLayer;
        }
        json::Value tex = json::Value::object();
        for (const auto& [slot, path] : r.textures) {
            tex[slot] = path;
        }
        o["textures"] = std::move(tex);
        reps.push(std::move(o));
    }
    man["replacements"] = std::move(reps);
    const std::string manText = json::writePretty(man);
    files[kManifestFile] = std::vector<std::uint8_t>(manText.begin(), manText.end());

    for (const auto& [rel, bytes] : files) {
        if (!store::writeFileAtomic(options.outDir / rel, bytes, &err)) {
            rep.errors.push_back(err);
            return rep;
        }
        rep.files.push_back(rel);
    }
    rep.replacements = std::move(all);
    rep.ok = true;
    return rep;
}

CheckReport checkExport(const fs::path& modDir, const fs::path& projectDir) {
    CheckReport rep;
    std::string err;
    auto project = detail::loadProject(projectDir, &err);
    if (!project) {
        rep.errors.push_back(err);
        return rep;
    }
    // 1. manifest schema
    const auto manText = store::readFileText(modDir / kManifestFile);
    const auto man = manText ? json::parse(*manText) : std::nullopt;
    if (!man || !man->get("schema") || man->get("schema")->asString() != kExportSchema) {
        rep.errors.push_back(std::string(kManifestFile) + ": missing or not a " + kExportSchema + " document");
        return rep;
    }
    static const std::regex hexRe("[0-9A-F]{16}");
    static const std::regex uuidRe("[0-9a-f]{8}-[0-9a-f]{4}-5[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}");
    const json::Value* mg = man->get("game_id");
    if (!mg || mg->asString() != project->gameId || !man->get("mod") || man->get("mod")->asString() != "mod.usda") {
        rep.errors.push_back("manifest: game_id must be the project's and mod must be \"mod.usda\"");
    }
    std::set<std::string> expected;
    std::map<std::string, std::pair<std::string, std::string>> byPrim; // prim -> (kind, hash)
    std::set<std::string> referenced{"mod.usda", kManifestFile};
    const json::Value* reps = man->get("replacements");
    if (!reps || !reps->isArray() || reps->size() == 0) {
        rep.errors.push_back("manifest: 'replacements' must be a non-empty array");
        return rep;
    }
    for (const json::Value& r : reps->asArray()) {
        auto s = [&](const char* k) { return r.get(k) ? r.get(k)->asString() : std::string(); };
        const std::string kind = s("kind"), hash = s("remix_hash"), prim = s("prim"), oaid = s("oaid");
        const std::string want = kind == "mesh" ? "/RootNode/meshes/mesh_" + hash : "/RootNode/Looks/mat_" + hash;
        if ((kind != "mesh" && kind != "material") || !std::regex_match(hash, hexRe) || prim != want ||
            !std::regex_match(oaid, uuidRe) || (s("distribution") != "ship" && s("distribution") != "local-only") ||
            !r.get("textures") || !r.get("textures")->isObject() || (kind == "mesh") != !s("mesh_layer").empty()) {
            rep.errors.push_back("manifest: malformed replacement entry for '" + prim + "'");
            continue;
        }
        const auto key = project->database->resolve(kind == "mesh" ? "remix.geom.asset" : "remix.tex", hash);
        if (!key || *key != oaid) {
            rep.errors.push_back("manifest: " + prim + " does not resolve to oaid " + oaid + " in the project");
        }
        expected.insert(oaid);
        byPrim[prim] = {kind, hash};
        if (kind == "mesh") {
            referenced.insert(s("mesh_layer"));
        }
        for (const auto& [slot, path] : r.get("textures")->asObject()) {
            referenced.insert(path.asString());
            std::string derr;
            const auto dds = rast::readDdsFile(modDir / path.asString(), &derr);
            const bool bc = dds && (dds->image.format == rast::TexFormat::BC7_SRGB_BLOCK || dds->image.format == rast::TexFormat::BC7_UNORM_BLOCK ||
                                    dds->image.format == rast::TexFormat::BC5_UNORM_BLOCK);
            if (!bc) {
                rep.errors.push_back(path.asString() + ": not a readable BC7 / BC5 DDS (" + derr + ")");
            }
        }
    }
    if (!byPrim.empty() && std::any_of(byPrim.begin(), byPrim.end(), [](const auto& kv) { return kv.second.first == "material"; })) {
        referenced.insert("materials/materials.usda");
    }

    // 2. composed stage: every replacement classifies with its hash; no error diagnostics.
    const rusd::ComposedStage stage = rusd::readStage((modDir / "mod.usda").generic_string());
    if (!stage.ok) {
        rep.errors.push_back("mod.usda: the stage does not load");
    }
    for (const rusd::Diagnostic& d : stage.diagnostics) {
        if (d.severity == rusd::Diagnostic::Severity::Error) {
            rep.errors.push_back("mod.usda: " + d.code + " " + d.primPath + ": " + d.message);
        }
    }
    const rusd::RemixMod remix = rusd::collectRemixMod(stage);
    for (const auto& [prim, kh] : byPrim) {
        bool found = false;
        if (kh.first == "mesh") {
            for (const auto& m : remix.meshes) {
                found = found || (m.path == prim && !m.meshPrims.empty());
            }
        } else {
            for (const auto& m : remix.materials) {
                found = found || (m.path == prim && m.mdlSourceAsset.find("AperturePBR_Opacity") != std::string::npos);
            }
        }
        if (!found) {
            rep.errors.push_back("mod.usda: " + prim + " is not a " + kh.first + " replacement of the composed stage");
        }
    }

    // 3. re-ingest the mod: every exported oaid has a replacement row.
    rimp::ImportOptions io;
    io.root = modDir.generic_string();
    io.gameId = project->gameId;
    io.kind = rimp::ImportKind::Mod;
    const rimp::ImportResult re = rimp::importMod(io);
    std::set<std::string> found;
    if (const auto* rows = re.db.get("replacement")) {
        for (const auto& row : rows->a) {
            found.insert(row.str("oaid"));
        }
    }
    for (const std::string& oaid : expected) {
        if (!found.count(oaid)) {
            rep.errors.push_back("re-ingest: no replacement row for " + oaid);
        }
    }
    for (const std::string& oaid : found) {
        rep.foundOaids.push_back(oaid);
    }
    if (re.errors() > 0) {
        for (const auto& d : re.diagnostics) {
            if (d.severity == "error") {
                rep.errors.push_back("re-ingest: " + d.code + " " + d.where + ": " + d.message);
            }
        }
    }

    // 4. lint: no original blob, no original pixels, no path out of the mod folder, no unreferenced file.
    const store::BlobStore blobs(project->store);
    std::set<std::string> originalBlobs;
    for (const std::string& rel : blobs.listRecords()) {
        const auto text = store::readFileText(blobs.root() / rel);
        const auto rec = text ? poco::fromJsonText(*text) : std::nullopt;
        if (rec && (isOriginalRecord(*rec) || isNotShippable(*rec))) {
            collectBlobs(*rec, originalBlobs);
        }
    }
    std::set<std::string> originalPixels;
    for (const db::HashKey& k : project->database->hashKeys()) {
        if (k.algo == "fuse.capture.sha256" || k.algo == "sha256") {
            originalPixels.insert(k.value);
        }
    }
    static const std::regex assetRe("@([^@]*)@");
    std::error_code ec;
    for (fs::recursive_directory_iterator it(modDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) {
            continue;
        }
        const std::string rel = fs::relative(it->path(), modDir).generic_string();
        const auto bytes = store::readFileBytes(it->path());
        if (!bytes) {
            continue;
        }
        const std::string sha = sha256Hex(bytes->data(), bytes->size());
        if (originalBlobs.count(sha)) {
            rep.errors.push_back("lint: " + rel + " is a blob of an original / non-shippable record of the project store");
        }
        if (!referenced.count(rel)) {
            rep.errors.push_back("lint: " + rel + " is not part of the export (stray file)");
        }
        if (rel.ends_with(".dds")) {
            const auto dds = rast::readDds(*bytes);
            if (dds && (dds->image.format == rast::TexFormat::BC7_SRGB_BLOCK || dds->image.format == rast::TexFormat::BC7_UNORM_BLOCK)) {
                const rast::Subresource* s0 = dds->image.subresource(0, 0, 0);
                cook::BcSourceImage dec;
                if (s0 && cook::decode_bc_image(cook::BcFormat::BC7, dds->image.data.data() + s0->offset, s0->size, s0->width, s0->height, dec) &&
                    originalPixels.count(store::canonicalTextureSha256(dec.rgba8))) {
                    rep.errors.push_back("lint: " + rel + " decodes to the pixels of a captured original texture");
                }
            }
        }
        if (rel.ends_with(".usda")) {
            const std::string text(bytes->begin(), bytes->end());
            for (std::sregex_iterator m(text.begin(), text.end(), assetRe), mend; m != mend; ++m) {
                const std::string path = (*m)[1].str();
                if (path == "AperturePBR_Opacity.mdl" || path == "AperturePBR_Translucent.mdl") {
                    continue; // NVIDIA's MDL modules are referenced by name only, never shipped
                }
                const fs::path resolved = (it->path().parent_path() / path).lexically_normal();
                const std::string inside = fs::relative(resolved, modDir, ec).generic_string();
                if (fs::path(path).is_absolute() || inside.empty() || inside.starts_with("..") || !fs::exists(resolved, ec)) {
                    rep.errors.push_back("lint: " + rel + " references '" + path + "' outside the mod folder (or missing)");
                }
            }
        }
    }
    return rep;
}

} // namespace fuse::remaster::remix
