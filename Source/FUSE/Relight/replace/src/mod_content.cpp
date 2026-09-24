// FUSE Relight RL-3.4: one mod's replacements (see mod_content.hpp).
#include <fuse/relight/replace/mod_content.hpp>

#include <fuse/relight/capture/export/capture_model.hpp>
#include <fuse/relight/capture/export/poco_store.hpp>
#include <fuse/relight/hash/hash_string.hpp>
#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/mods/import/mod_importer.hpp>
#include <fuse/relight/mods/usd/remix_profile.hpp>
#include <fuse/relight/options/option_config.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

namespace fuse::relight::replace {

namespace fs = std::filesystem;
namespace ex = capture::exporter;
using ex::json::Value;

Mat4d identity4d() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4d multiply(const Mat4d& a, const Mat4d& b) {
    Mat4d r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                s += a[i * 4 + k] * b[k * 4 + j];
            }
            r[i * 4 + j] = s;
        }
    }
    return r;
}

Mat4d toMat4d(const std::array<float, 16>& m) {
    Mat4d r{};
    for (std::size_t i = 0; i < 16; ++i) {
        r[i] = double(m[i]);
    }
    return r;
}

Vec3d transformPoint(const Mat4d& m, const Vec3d& p) {
    Vec3d r{};
    for (int j = 0; j < 3; ++j) {
        r[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + m[3 * 4 + j];
    }
    return r;
}

Vec3d transformDirection(const Mat4d& m, const Vec3d& d) {
    Vec3d r{};
    for (int j = 0; j < 3; ++j) {
        r[j] = d[0] * m[0 * 4 + j] + d[1] * m[1 * 4 + j] + d[2] * m[2 * 4 + j];
    }
    const double len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!(len > 0.0)) {
        return d;
    }
    return {r[0] / len, r[1] / len, r[2] / len};
}

const char* modKindName(ModKind kind) { return kind == ModKind::FuseNative ? "fuse" : "remix"; }
const char* modFormatName(ModFormat format) { return format == ModFormat::Store ? "store" : "usd"; }

namespace {

constexpr const char* kPocoRef = "poco:";

std::string stripPoco(const std::string& ref) {
    return ref.compare(0, 5, kPocoRef) == 0 ? ref.substr(5) : ref;
}

std::string recordPath(const std::string& kind, const std::string& id) { return "poco/" + kind + "/" + id + ".poco.json"; }

Mat4d matrixOf(const Value* v) {
    Mat4d m = identity4d();
    if (v && v->isArray() && v->a.size() == 16) {
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = v->a[i].n;
        }
    }
    return m;
}

Vec3d vec3Of(const Value* v, Vec3d fallback) {
    if (v && v->isArray() && v->a.size() >= 3) {
        return {v->a[0].n, v->a[1].n, v->a[2].n};
    }
    return fallback;
}

std::optional<scene::InstanceCategories> categoryByName(const std::string& name) {
    for (std::uint32_t i = 0; i < scene::kInstanceCategoryCount; ++i) {
        const auto c = static_cast<scene::InstanceCategories>(i);
        if (name == scene::instanceCategoryName(c)) {
            return c;
        }
    }
    return std::nullopt;
}

std::optional<Hash64> parseHex(const std::string& s) {
    if (s.empty()) {
        return std::nullopt;
    }
    return hash::parseHashOption(s);
}

/// The parser's view of the store: reads + parses records, and fingerprints what it read.
class Reader {
public:
    Reader(const StoreReader& read, ModContent& out) : m_read(read), m_out(out) {}

    const Value* record(const std::string& kind, const std::string& id, Hash64* fingerprint) {
        const std::string path = recordPath(kind, id);
        auto it = m_cache.find(path);
        if (it == m_cache.end()) {
            std::optional<Value> parsed;
            Hash64 fp = 0;
            if (auto bytes = m_read(path)) {
                std::string err;
                parsed = ex::json::parse(*bytes, &err);
                fp = hash::xxh64(bytes->data(), bytes->size(), 0);
                if (!parsed) {
                    diag("error", "bad_record", path, "record does not parse: " + err);
                }
            } else {
                diag("error", "missing_record", path, "record named by the DB is missing");
            }
            it = m_cache.emplace(path, Entry{std::move(parsed), fp}).first;
        }
        if (fingerprint) {
            *fingerprint = hash::xxh64(&it->second.fingerprint, sizeof it->second.fingerprint, *fingerprint);
        }
        return it->second.value ? &*it->second.value : nullptr;
    }

    std::optional<std::string> blob(const Value* ref) {
        if (!ref || !ref->isObject()) {
            return std::nullopt;
        }
        const std::string sha = ref->str("sha256");
        if (sha.size() < 3) {
            return std::nullopt;
        }
        return m_read("blobs/sha256/" + sha.substr(0, 2) + "/" + sha);
    }

    void diag(const char* severity, const std::string& code, const std::string& where, const std::string& message) {
        m_out.diagnostics.push_back({severity, code, where, message});
    }

private:
    struct Entry {
        std::optional<Value> value;
        Hash64 fingerprint = 0;
    };
    const StoreReader& m_read;
    ModContent& m_out;
    std::map<std::string, Entry> m_cache;
};

LightDef lightDefOf(const std::string& id, const Value& rec) {
    LightDef l;
    l.recordId = id;
    l.name = rec.str("name");
    const Value* p = rec.get("payload");
    if (!p || !p->isObject()) {
        return l;
    }
    l.type = p->str("type");
    l.color = vec3Of(p->get("colorLinear"), l.color);
    l.intensity = p->num("intensity");
    l.innerCone = p->num("innerCone");
    l.outerCone = p->num("outerCone");
    if (const Value* s = p->get("size"); s && s->isArray() && s->a.size() >= 2) {
        l.size = {s->a[0].n, s->a[1].n};
    }
    if (const Value* rl = p->get("relight"); rl && rl->isObject()) {
        l.usdType = rl->str("usd_type");
        l.transform = matrixOf(rl->get("transform"));
    }
    return l;
}

MaterialDef materialDefOf(Reader& reader, const std::string& id) {
    MaterialDef m;
    m.recordId = id;
    const Value* base = reader.record("material", id, &m.fingerprint);
    const Value* ext = reader.record("material_ext", id, &m.fingerprint);
    if (base) {
        m.name = base->str("name");
        if (const Value* p = base->get("payload"); p && p->isObject()) {
            m.model = p->str("model");
            if (const Value* c = p->get("baseColor"); c && c->isArray() && c->a.size() >= 4) {
                m.baseColor = {c->a[0].n, c->a[1].n, c->a[2].n, c->a[3].n};
            }
            m.roughness = p->num("roughness", m.roughness);
            m.metallic = p->num("metallic", m.metallic);
            m.emissiveNits = p->num("emissiveNits", m.emissiveNits);
        }
    }
    if (ext) {
        if (const Value* p = ext->get("payload"); p && p->isObject()) {
            m.surface = p->str("surface");
            m.ignoreMaterial = p->flag("ignore_material");
            m.preloadTextures = p->flag("preload_textures");
            if (auto h = parseHex(p->str("remix_hash"))) {
                m.textureHash = *h;
            }
            if (const Value* t = p->get("textures"); t && t->isObject()) {
                for (const auto& [param, x] : t->o) {
                    if (!x.isObject() || x.str("status") != "ok") {
                        continue;
                    }
                    TextureRef r;
                    r.param = param;
                    if (const Value* b = x.get("blob"); b && b->isObject()) {
                        r.sha256 = b->str("sha256");
                        r.fileBytes = static_cast<std::uint64_t>(b->num("size"));
                    }
                    r.format = x.str("format");
                    r.width = x.u32("width");
                    r.height = x.u32("height");
                    r.mips = std::max<std::uint32_t>(1, x.u32("mips", 1));
                    r.srgb = x.flag("srgb");
                    if (!r.sha256.empty()) {
                        m.textures.push_back(std::move(r));
                    }
                }
            }
        }
    }
    return m;
}

} // namespace

std::optional<std::int64_t> modPriorityFromConf(const std::string& rtxConf) {
    if (rtxConf.empty()) {
        return std::nullopt;
    }
    options::ConfigParseOptions po;
    po.exeName = "\x01"; // no [section] applies: a mod's rtx.conf is not per executable
    const options::OptionConfig c = options::OptionConfig::parse(rtxConf, po);
    for (const char* key : {"relight.mod.priority", "rtx.mod.priority"}) {
        if (const std::string* v = c.find(key)) {
            try {
                std::size_t used = 0;
                const long long n = std::stoll(*v, &used, 10);
                if (used == v->size()) {
                    return n;
                }
            } catch (...) {
            }
        }
    }
    return std::nullopt;
}

ModContent parseModStore(const ModLocation& location, const StoreReader& read, const Value& db, const std::string& source) {
    ModContent out;
    out.location = location;
    out.idPrefix = source;
    Reader reader(read, out);
    if (!db.isObject() || db.str("schema") != ex::kDbSchema) {
        reader.diag("error", "bad_db", "db/remaster_db.json", "not a " + std::string(ex::kDbSchema) + " document");
        return out;
    }
    out.ok = true;

    // The mod record: rtx.conf, rule, diagnostics (light deletions).
    if (!source.empty()) {
        if (const Value* mod = reader.record("relight_mod", source, nullptr)) {
            if (const Value* p = mod->get("payload"); p && p->isObject()) {
                if (auto conf = reader.blob(p->get("rtx_conf"))) {
                    out.rtxConf = *conf;
                }
                out.assetRule = p->str("geometry_asset_rule");
                if (const Value* diags = p->get("diagnostics"); diags && diags->isArray()) {
                    for (const Value& d : diags->a) {
                        out.diagnostics.push_back({d.str("severity"), d.str("code"), d.str("where"), d.str("message")});
                        if (d.str("code") != "not_a_light") {
                            continue;
                        }
                        const mods::usd::Classification c = mods::usd::classifyPrimPath(d.str("where"));
                        if (c.cls != mods::usd::PrimClass::Light || c.hash == 0) {
                            continue;
                        }
                        LightReplacementDef del;
                        del.hash = c.hash;
                        del.deleted = true;
                        del.primPath = d.str("where");
                        del.fingerprint = hash::xxh64(del.primPath.data(), del.primPath.size(), 1);
                        out.lights.emplace(c.hash, std::move(del));
                    }
                }
            }
        }
    }
    out.priority = modPriorityFromConf(out.rtxConf);

    std::vector<std::pair<std::string, std::string>> rows; // (kind, poco id)
    if (const Value* rp = db.get("replacement"); rp && rp->isArray()) {
        for (const Value& row : rp->a) {
            if (!source.empty() && row.str("source") != source) {
                continue;
            }
            if (row.str("variant", "default") != "default") {
                continue; // variants other than the default belong to tier selection (RL-4.x)
            }
            rows.emplace_back(row.str("kind"), row.str("poco_id"));
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    for (const auto& [kind, id] : rows) {
        if (kind == "relight_replacement") {
            MeshReplacementDef m;
            m.recordId = id;
            const Value* rec = reader.record(kind, id, &m.fingerprint);
            const Value* p = rec ? rec->get("payload") : nullptr;
            if (!p || !p->isObject()) {
                continue;
            }
            const Value* key = p->get("key");
            if (!key || !key->isObject()) {
                reader.diag("error", "bad_record", recordPath(kind, id), "relight_replacement without a key");
                continue;
            }
            m.algo = key->str("algo");
            m.ruleString = key->str("rule");
            m.rule = hash::parseHashRule(m.ruleString);
            if (m.algo == ex::key_algo::kGeomLegacy0) {
                m.rule = hash::rules::kLegacyAsset0;
            } else if (m.algo == ex::key_algo::kGeomLegacy1) {
                m.rule = hash::rules::kLegacyAsset1;
            }
            const auto h = parseHex(key->str("value"));
            if (!h || m.rule.empty()) {
                reader.diag("error", "bad_record", recordPath(kind, id), "relight_replacement key does not parse");
                continue;
            }
            m.key = *h;
            if (const Value* pr = p->get("preserveOriginalDrawCall"); pr && pr->kind == Value::Kind::Bool) {
                m.preserveOriginalDrawCall = pr->b;
            }
            if (const Value* cats = p->get("categories"); cats && cats->isObject()) {
                for (const char* which : {"set", "cleared"}) {
                    if (const Value* list = cats->get(which); list && list->isArray()) {
                        for (const Value& n : list->a) {
                            if (auto c = categoryByName(n.s)) {
                                (which[0] == 's' ? m.categoriesSet : m.categoriesCleared).set(*c);
                            }
                        }
                    }
                }
            }
            if (const Value* parts = p->get("parts"); parts && parts->isArray()) {
                for (const Value& part : parts->a) {
                    MeshPartDef d;
                    d.meshId = stripPoco(part.str("mesh"));
                    d.prim = part.str("prim");
                    d.transform = matrixOf(part.get("transform"));
                    if (const Value* mats = part.get("materials"); mats && mats->isArray()) {
                        for (const Value& mv : mats->a) {
                            d.materials.push_back(stripPoco(mv.s));
                        }
                    }
                    if (const Value* mesh = reader.record("mesh", d.meshId, &m.fingerprint)) {
                        if (const Value* mp = mesh->get("payload")) {
                            if (const Value* b = mp->get("bounds"); b && b->isArray() && b->a.size() >= 6) {
                                for (std::size_t i = 0; i < 6; ++i) {
                                    d.bounds[i] = b->a[i].n;
                                }
                            }
                        }
                    }
                    for (const std::string& mid : d.materials) {
                        if (!mid.empty() && !out.boundMaterials.count(mid)) {
                            out.boundMaterials.emplace(mid, materialDefOf(reader, mid));
                        }
                    }
                    m.parts.push_back(std::move(d));
                }
            }
            if (const Value* lights = p->get("lights"); lights && lights->isArray()) {
                for (const Value& lv : lights->a) {
                    const std::string lid = stripPoco(lv.s);
                    if (const Value* lr = reader.record("light", lid, &m.fingerprint)) {
                        m.lights.push_back(lightDefOf(lid, *lr));
                    }
                }
            }
            out.meshes[{m.rule.bits, m.key}] = std::move(m);
        } else if (kind == "material") {
            MaterialDef m = materialDefOf(reader, id);
            if (m.textureHash == 0) {
                continue;
            }
            out.boundMaterials[id] = m;
            out.materials[m.textureHash] = std::move(m);
        } else if (kind == "light") {
            LightReplacementDef l;
            const Value* rec = reader.record(kind, id, &l.fingerprint);
            if (!rec) {
                continue;
            }
            l.light = lightDefOf(id, *rec);
            if (const Value* p = rec->get("payload"); p && p->isObject()) {
                if (const Value* rl = p->get("relight"); rl && rl->isObject()) {
                    if (auto h = parseHex(rl->str("remix_hash"))) {
                        l.hash = *h;
                    }
                }
            }
            if (l.hash == 0) {
                continue;
            }
            if (const Value* prov = rec->get("provenance")) {
                if (const Value* src = prov->get("source")) {
                    l.primPath = src->str("prim");
                }
            }
            out.lights[l.hash] = std::move(l); // an actual light wins over a deletion of the same hash
        }
    }
    return out;
}

ModContent loadUsdMod(const ModLocation& location, const std::string& gameId) {
    mods::import::ImportOptions opt;
    opt.root = location.dir;
    opt.gameId = gameId.empty() ? "game" : gameId;
    opt.modName = location.name;
    opt.kind = mods::import::ImportKind::Mod;
    const mods::import::ImportResult r = mods::import::importMod(opt);
    if (!r.ok) {
        ModContent out;
        out.location = location;
        for (const mods::import::ImportDiagnostic& d : r.diagnostics) {
            out.diagnostics.push_back({d.severity, d.code, d.where, d.message});
        }
        if (out.diagnostics.empty()) {
            out.diagnostics.push_back({"error", "import_failed", location.dir, "the mod could not be imported"});
        }
        return out;
    }
    const auto* files = &r.files;
    StoreReader read = [files](const std::string& rel) -> std::optional<std::string> {
        const auto it = files->find(rel);
        if (it == files->end()) {
            return std::nullopt;
        }
        return std::string(it->second.begin(), it->second.end());
    };
    ModContent out = parseModStore(location, read, r.db, r.idPrefix);
    return out;
}

namespace {

std::optional<Value> readDb(const std::string& dir) {
    std::string text;
    if (!ex::readFile(fs::path(dir) / "db" / "remaster_db.json", text)) {
        return std::nullopt;
    }
    return ex::json::parse(text);
}

StoreReader diskReader(const std::string& dir) {
    return [dir](const std::string& rel) -> std::optional<std::string> {
        std::string text;
        if (rel.find("..") != std::string::npos || !ex::readFile(fs::path(dir) / rel, text)) {
            return std::nullopt;
        }
        return text;
    };
}

} // namespace

std::vector<std::string> storeSources(const std::string& dir) {
    std::set<std::string> sources;
    if (const auto db = readDb(dir)) {
        if (const Value* rp = db->get("replacement"); rp && rp->isArray()) {
            for (const Value& row : rp->a) {
                if (!row.str("source").empty()) {
                    sources.insert(row.str("source"));
                }
            }
        }
    }
    return {sources.begin(), sources.end()};
}

ModContent loadStoreMod(const ModLocation& location) {
    const auto db = readDb(location.dir);
    if (!db) {
        ModContent out;
        out.location = location;
        out.diagnostics.push_back({"error", "no_db", location.dir, "db/remaster_db.json is missing or does not parse"});
        return out;
    }
    return parseModStore(location, diskReader(location.dir), *db, location.source);
}

ModContent loadMod(const ModLocation& location, const std::string& gameId) {
    return location.format == ModFormat::Store ? loadStoreMod(location) : loadUsdMod(location, gameId);
}

} // namespace fuse::relight::replace
