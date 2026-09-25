// fuse_lint asset-licences: FUSE_ASSET_PLAN §2.4 (see fuse_lint_asset_licences.hpp for the rules).

#include "fuse_lint_asset_licences.hpp"

#include "fuse_json_mini.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace fuse::tools::lint {
namespace {

using Violations = std::vector<std::string>;

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeAll(const fs::path& p, std::string_view text) {
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string foldCrlf(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string lowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// ---- licence policy (§2.1, §2.3, REMASTER_PLAN §2.6) -------------------------------------------------

/// Ordered by restrictiveness; propagation takes the maximum over a derivation chain.
enum class LicClass : int {
    Permissive = 0,  ///< §2.1.1 allowed without review
    Attribution = 1, ///< §2.1.2 allowed with attribution tracking (credits line)
    Review = 2,      ///< §2.1.3 human review, no share-alike (AI outputs, rights-holder grants, custom CC0)
    Copyleft = 3,    ///< §2.1.3 share-alike / database / GPL art: human review + no flow into permissive assets
    NeverShip = 4,   ///< reference-only (originals, world-model / Veo / Genie outputs): review + distribution != ship
    Forbidden = 5,   ///< §2.1.4 NC, ND, editorial, royalty-free seat, research-only
    Unknown = 6,     ///< unknown provenance (NOASSERTION / NONE / LicenseRef-Unknown): always reported
    NotListed = 7,   ///< an identifier the policy does not know: not on the allow-list
};

const char* className(LicClass c) {
    switch (c) {
    case LicClass::Permissive: return "permissive";
    case LicClass::Attribution: return "attribution";
    case LicClass::Review: return "review-required";
    case LicClass::Copyleft: return "share-alike/copyleft (review-required)";
    case LicClass::NeverShip: return "reference-only (never shipped)";
    case LicClass::Forbidden: return "forbidden";
    case LicClass::Unknown: return "unknown provenance";
    case LicClass::NotListed: return "not on the allow-list";
    }
    return "?";
}

LicClass classify(const std::string& id) {
    static const std::set<std::string> permissive = {"CC0-1.0", "Unlicense", "LicenseRef-PublicDomain",
                                                     "LicenseRef-PublicDomain-USGov", "LicenseRef-FUSE-Generated"};
    static const std::set<std::string> attribution = {"CC-BY-3.0", "CC-BY-4.0", "ODC-By-1.0", "LicenseRef-Copernicus-DEM",
                                                      "LicenseRef-OGA-BY-3.0", "MIT", "BSD-2-Clause", "BSD-3-Clause",
                                                      "Apache-2.0", "Zlib"};
    static const std::set<std::string> copyleft = {"CC-BY-SA-3.0", "CC-BY-SA-4.0", "ODbL-1.0", "GPL-2.0-only",
                                                   "GPL-2.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later",
                                                   "LGPL-2.1-or-later", "LGPL-3.0-or-later"};
    static const std::set<std::string> review = {"LicenseRef-AI-Gemini", "LicenseRef-AI-Gemini-Image",
                                                 "LicenseRef-CC0-Restricted"};
    static const std::set<std::string> never = {"LicenseRef-AI-Veo", "LicenseRef-AI-Genie-Reference",
                                                "LicenseRef-AI-WorldModel-Reference"};
    if (id.empty() || id == "NOASSERTION" || id == "NONE" || startsWith(id, "LicenseRef-Unknown")) {
        return LicClass::Unknown;
    }
    const std::string up = [&] {
        std::string u = id;
        std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return u;
    }();
    if (up.find("-NC") != std::string::npos || up.find("-ND") != std::string::npos ||
        up.find("NONCOMMERCIAL") != std::string::npos || startsWith(id, "LicenseRef-Editorial") ||
        startsWith(id, "LicenseRef-RoyaltyFree") || startsWith(id, "LicenseRef-Research")) {
        return LicClass::Forbidden;
    }
    if (permissive.count(id)) return LicClass::Permissive;
    if (attribution.count(id)) return LicClass::Attribution;
    if (copyleft.count(id)) return LicClass::Copyleft;
    if (review.count(id) || startsWith(id, "LicenseRef-RightsHolderGrant-") || startsWith(id, "LicenseRef-CC0-Restricted-")) {
        return LicClass::Review;
    }
    if (never.count(id) || startsWith(id, "LicenseRef-Original-")) return LicClass::NeverShip;
    return LicClass::NotListed;
}

/// Research-only / non-commercial datasets named in §2.2 / §2.3 that must never enter the cook.
bool mentionsResearchDataset(const JsonValue& rec) {
    static const char* names[] = {"bandai namco", "bandai-namco", "bandainamco", "amass", "mixamo"};
    for (const char* key : {"id", "source", "source_id", "url"}) {
        const std::string v = lowerStr(rec[key].asString());
        for (const char* n : names) {
            if (!v.empty() && v.find(n) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

struct Record {
    const JsonValue* json = nullptr;
    std::string id;
    std::string licence;
    LicClass cls = LicClass::NotListed;
    std::string distribution = "ship";
};

std::string cell(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    std::string out;
    for (char c : s) {
        if (c == '|') out += "\\|";
        else out.push_back(c);
    }
    return out.empty() ? "-" : out;
}

bool loadLock(const fs::path& lockFile, JsonValue& doc, std::string& error) {
    if (!fs::is_regular_file(lockFile)) {
        error = lockFile.generic_string() + ": missing licence lock file";
        return false;
    }
    std::string perr;
    if (!parseJson(readAll(lockFile), doc, perr)) {
        error = lockFile.generic_string() + ": JSON parse error: " + perr;
        return false;
    }
    return true;
}

bool isRootManifestFile(const std::string& rel) {
    return rel == "licences.lock.json" || rel == "CREDITS.md" || rel == "sources.lock.json";
}

} // namespace

bool renderAssetCredits(const fs::path& lockFile, std::string& out, std::string& error) {
    JsonValue doc;
    if (!loadLock(lockFile, doc, error)) {
        return false;
    }
    std::vector<const JsonValue*> recs;
    for (const JsonValue& r : doc["assets"].arr) {
        recs.push_back(&r);
    }
    std::sort(recs.begin(), recs.end(), [](const JsonValue* a, const JsonValue* b) {
        return (*a)["id"].asString() < (*b)["id"].asString();
    });
    std::ostringstream md;
    md << "# FUSE content credits\n\n"
       << "<!-- Generated from Content/licences.lock.json by `fuse_lint asset-credits --root Content`. Do not edit;\n"
       << "     the fuse_lint_asset_licences gate fails when this file is stale. -->\n\n"
       << "## Attribution required\n\n";
    size_t nAttr = 0;
    std::ostringstream attr;
    for (const JsonValue* r : recs) {
        const std::string lic = (*r)["licence"].asString();
        const std::string line = (*r)["attribution"].asString();
        if (classify(lic) != LicClass::Attribution && line.empty()) {
            continue;
        }
        ++nAttr;
        attr << "| " << cell((*r)["id"].asString()) << " | " << cell(line) << " | " << cell((*r)["author"].asString())
             << " | " << cell((*r)["source"].asString()) << " | " << cell((*r)["url"].asString()) << " | " << cell(lic)
             << " |\n";
    }
    if (nAttr == 0u) {
        md << "_None._\n\n";
    } else {
        md << "| Asset | Attribution | Author | Source | URL | Licence |\n|---|---|---|---|---|---|\n" << attr.str() << "\n";
    }
    md << "## All assets\n\n";
    if (recs.empty()) {
        md << "_None._\n";
    } else {
        md << "| Asset | Origin | Licence | Author | Source | Distribution |\n|---|---|---|---|---|---|\n";
        for (const JsonValue* r : recs) {
            md << "| " << cell((*r)["id"].asString()) << " | " << cell((*r)["origin"].asString()) << " | "
               << cell((*r)["licence"].asString()) << " | " << cell((*r)["author"].asString()) << " | "
               << cell((*r)["source"].asString()) << " | " << cell((*r)["distribution"].asString("ship")) << " |\n";
        }
    }
    out = md.str();
    return true;
}

Violations checkAssetLicences(const fs::path& content, const fs::path& manifest, const fs::path& cache, const Sha256Fn& sha256) {
    Violations v;
    const fs::path lockFile = content / "licences.lock.json";
    const std::string lockName = lockFile.generic_string();
    JsonValue doc;
    std::string err;
    if (!loadLock(lockFile, doc, err)) {
        v.push_back(err);
        return v;
    }
    if (doc["schema"].asNumber(-1.0) != 1.0) {
        v.push_back(lockName + ": 'schema' must be 1");
    }
    if (!doc["assets"].isArray()) {
        v.push_back(lockName + ": 'assets' must be an array");
        return v;
    }

    static const std::regex idRe(R"([a-z0-9][a-z0-9_./-]*)");
    static const std::regex dateRe(R"([0-9]{4}-[0-9]{2}-[0-9]{2})");
    static const std::regex generatorRe(R"(\S+@[0-9a-f]{7,40})");
    static const std::regex shaRe(R"([0-9a-f]{64})");
    static const std::set<std::string> origins = {"sourced", "generated", "derived", "original", "derived-original", "ai", "unknown"};
    static const std::set<std::string> retrievals = {"direct-download", "api", "mirror", "generator"};
    static const std::set<std::string> distributions = {"ship", "recipe-only", "never"};

    std::map<std::string, Record> recs;
    std::map<std::string, std::string> fileOwner; // Content-relative path -> record id
    for (const JsonValue& r : doc["assets"].arr) {
        Record rec;
        rec.json = &r;
        rec.id = r["id"].asString();
        const std::string where = lockName + ": asset '" + rec.id + "'";
        if (!r.isObject() || rec.id.empty() || !std::regex_match(rec.id, idRe)) {
            v.push_back(lockName + ": record with missing or malformed id '" + rec.id + "'");
            continue;
        }
        if (recs.count(rec.id)) {
            v.push_back(where + ": duplicate id");
            continue;
        }
        const std::string origin = r["origin"].asString();
        rec.licence = r["licence"].asString();
        rec.cls = classify(rec.licence);
        rec.distribution = r["distribution"].asString("ship");
        if (!origins.count(origin)) {
            v.push_back(where + ": origin '" + origin + "' is not sourced|generated|derived|original|derived-original|ai|unknown");
        }
        // Rule 5 + unknown provenance: reported, never accepted.
        if (origin == "unknown" || rec.cls == LicClass::Unknown) {
            v.push_back(where + ": unknown provenance (origin '" + origin + "', licence '" + rec.licence +
                        "'): find the source and licence, or remove the asset");
        } else if (rec.cls == LicClass::Forbidden) {
            v.push_back(where + ": forbidden licence '" + rec.licence + "' (NC / ND / editorial / royalty-free)");
        } else if (rec.cls == LicClass::NotListed) {
            v.push_back(where + ": licence '" + rec.licence + "' is not on the allow-list (FUSE_ASSET_PLAN §2.1)");
        }
        if (mentionsResearchDataset(r)) {
            v.push_back(where + ": names a research-only / non-commercial dataset (Bandai Namco, AMASS, Mixamo; §2.3)");
            rec.cls = LicClass::Forbidden;
        }
        if (origin != "unknown") {
            for (const char* key : {"source", "author"}) {
                if (r[key].asString().empty()) {
                    v.push_back(where + ": required field '" + std::string(key) + "' missing or empty");
                }
            }
        }
        if (origin == "sourced") {
            if (!startsWith(r["url"].asString(), "https://")) {
                v.push_back(where + ": sourced asset needs an https 'url' (got '" + r["url"].asString() + "')");
            }
            if (!std::regex_match(r["retrieved"].asString(), dateRe)) {
                v.push_back(where + ": sourced asset needs 'retrieved' as YYYY-MM-DD");
            }
            if (!retrievals.count(r["retrieval"].asString())) {
                v.push_back(where + ": 'retrieval' must be direct-download|api|mirror|generator");
            }
        }
        // Rule 7: generated assets rebuild bit for bit from generator@revision + seed.
        if (origin == "generated" || origin == "ai") {
            if (!std::regex_match(r["generator"].asString(), generatorRe)) {
                v.push_back(where + ": generator record needs 'generator' as <path>@<git revision> (got '" +
                            r["generator"].asString() + "')");
            }
            if (!r["seed"].isNumber()) {
                v.push_back(where + ": generator record needs a numeric 'seed'");
            }
        }
        if ((origin == "derived" || origin == "derived-original") && r["derived_from"].arr.empty()) {
            v.push_back(where + ": derived asset needs a non-empty 'derived_from'");
        }
        if (rec.cls == LicClass::Attribution && r["attribution"].asString().empty()) {
            v.push_back(where + ": licence '" + rec.licence + "' requires an 'attribution' line for CREDITS.md");
        }
        // Rule 3: review-class licences need a signed review; a requested review must be signed too.
        const JsonValue& review = r["review"];
        const bool reviewSigned = review["required"].asBool() && !review["by"].asString().empty();
        const bool reviewClass = rec.cls == LicClass::Review || rec.cls == LicClass::Copyleft || rec.cls == LicClass::NeverShip;
        if (reviewClass && !reviewSigned) {
            v.push_back(where + ": licence '" + rec.licence + "' is " + className(rec.cls) +
                        ": needs review.required = true and review.by filled");
        } else if (!reviewClass && review["required"].asBool() && review["by"].asString().empty()) {
            v.push_back(where + ": review requested (review.required) but review.by is empty");
        }
        if (!distributions.count(rec.distribution)) {
            v.push_back(where + ": distribution '" + rec.distribution + "' is not ship|recipe-only|never");
        }
        if (rec.cls == LicClass::NeverShip && rec.distribution == "ship") {
            v.push_back(where + ": reference-only licence '" + rec.licence + "' cannot have distribution 'ship'");
        }
        // Files: Content-relative paths (location "content", default) or asset-cache paths ("cache").
        for (const JsonValue& f : r["files"].arr) {
            const std::string path = f["path"].asString();
            const std::string sha = f["sha256"].asString();
            const std::string loc = f["location"].asString("content");
            if (path.empty() || path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
                path.front() == '/') {
                v.push_back(where + ": file path '" + path + "' must be relative, '/'-separated, without '..'");
                continue;
            }
            if (!std::regex_match(sha, shaRe)) {
                v.push_back(where + ": file '" + path + "' needs a 64-hex lowercase sha256");
            }
            if (loc != "content" && loc != "cache") {
                v.push_back(where + ": file '" + path + "' location must be content|cache");
                continue;
            }
            fs::path onDisk;
            if (loc == "content") {
                if (fileOwner.count(path)) {
                    v.push_back(where + ": file '" + path + "' is already claimed by '" + fileOwner[path] + "'");
                }
                fileOwner[path] = rec.id;
                onDisk = content / path;
                if (!fs::is_regular_file(onDisk)) {
                    v.push_back(where + ": file '" + path + "' listed but missing under " + content.generic_string());
                    continue;
                }
            } else {
                if (cache.empty() || !fs::is_regular_file(cache / path)) {
                    continue; // cache not populated: bytes are checked when the cache is present
                }
                onDisk = cache / path;
            }
            const std::string bytes = readAll(onDisk);
            if (sha256(bytes) != sha && sha256(foldCrlf(bytes)) != sha) {
                v.push_back(where + ": sha256 mismatch for '" + path + "' (file is " + sha256(bytes) +
                            "; the asset changed: re-verify its provenance and update the lock)");
            }
        }
        recs.emplace(rec.id, rec);
    }

    // Rule 4: propagation over derived_from (transitive; cycles reported).
    std::map<std::string, LicClass> effective;
    std::map<std::string, std::string> worstInput;
    std::function<LicClass(const std::string&, std::set<std::string>&)> eff = [&](const std::string& id,
                                                                                  std::set<std::string>& stack) -> LicClass {
        if (auto it = effective.find(id); it != effective.end()) {
            return it->second;
        }
        const auto rit = recs.find(id);
        if (rit == recs.end()) {
            return LicClass::Permissive;
        }
        if (!stack.insert(id).second) {
            v.push_back(lockName + ": asset '" + id + "': derived_from cycle");
            return rit->second.cls;
        }
        LicClass worst = rit->second.cls;
        for (const JsonValue& in : rit->second.json->operator[]("derived_from").arr) {
            const std::string inId = in.asString();
            if (!recs.count(inId)) {
                continue;
            }
            const LicClass c = eff(inId, stack);
            if (int(c) > int(worst)) {
                worst = c;
                worstInput[id] = inId;
            }
        }
        stack.erase(id);
        effective[id] = worst;
        return worst;
    };
    for (const auto& [id, rec] : recs) {
        const std::string where = lockName + ": asset '" + id + "'";
        for (const JsonValue& in : rec.json->operator[]("derived_from").arr) {
            if (!recs.count(in.asString())) {
                v.push_back(where + ": derived_from '" + in.asString() + "' has no record");
            }
        }
        std::set<std::string> stack;
        eff(id, stack);
        const std::string via = worstInput.count(id) ? worstInput[id] : std::string();
        if (via.empty()) {
            continue; // its own class was already judged above
        }
        const LicClass inCls = effective[via];
        if (inCls == LicClass::Forbidden || inCls == LicClass::Unknown || inCls == LicClass::NotListed) {
            v.push_back(where + ": derived from '" + via + "' whose effective licence is " + className(inCls) +
                        " (forbidden inputs never flow into outputs)");
        } else if (inCls == LicClass::Copyleft && int(rec.cls) < int(LicClass::Copyleft)) {
            v.push_back(where + ": share-alike leakage: input '" + via + "' is " + className(inCls) + " but '" + id +
                        "' declares '" + rec.licence + "' (declare the strictest input licence)");
        } else if (inCls == LicClass::NeverShip && rec.distribution == "ship") {
            v.push_back(where + ": derived from reference-only '" + via + "' but has distribution 'ship'");
        }
    }
    for (const auto& [id, rec] : recs) {
        if (rec.json->operator[]("pack").asString() == "permissive" && int(effective[id]) >= int(LicClass::Copyleft)) {
            v.push_back(lockName + ": asset '" + id + "': marked for the permissive pack but its effective licence is " +
                        className(effective[id]));
        }
    }

    // Rule 1: every file under Content/ resolves to a record.
    std::error_code ec;
    for (fs::recursive_directory_iterator it(content, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) {
            continue;
        }
        const std::string relp = fs::relative(it->path(), content).generic_string();
        const std::string name = it->path().filename().string();
        if (isRootManifestFile(relp) || name == ".gitkeep" || name == ".gitignore") {
            continue;
        }
        if (!fileOwner.count(relp)) {
            v.push_back((content / relp).generic_string() + ": no licence record in licences.lock.json (add one with source, "
                        "licence, author, url and sha256; record an asset of unclear origin as origin \"unknown\")");
        }
    }

    // Rule 1 (cooked outputs): every cook-manifest entry names a licence record.
    if (!manifest.empty()) {
        JsonValue m;
        std::string perr;
        if (!fs::is_regular_file(manifest) || !parseJson(readAll(manifest), m, perr)) {
            v.push_back(manifest.generic_string() + ": cook manifest missing or unparsable " + perr);
        } else {
            for (const JsonValue& a : m["assets"].arr) {
                std::string lid = a["licence_id"].asString(a["licenceId"].asString());
                const std::string src = a["sourcePath"].asString(a["source_path"].asString());
                const std::string outp = a["outputPath"].asString(a["output_path"].asString());
                if (lid.empty() && fileOwner.count(src)) {
                    lid = fileOwner[src];
                }
                if (lid.empty() || !recs.count(lid)) {
                    v.push_back(manifest.generic_string() + ": cook manifest output '" + outp + "' (source '" + src +
                                "') resolves to no licence record (licence_id '" + lid + "')");
                }
            }
        }
    }

    // Rule 6: CREDITS.md is up to date.
    std::string credits;
    if (renderAssetCredits(lockFile, credits, err)) {
        const fs::path cf = content / "CREDITS.md";
        if (!fs::is_regular_file(cf) || foldCrlf(readAll(cf)) != credits) {
            v.push_back(cf.generic_string() + ": CREDITS.md is missing or stale; regenerate with `fuse_lint asset-credits --root " +
                        content.generic_string() + " --scratch <dir>`");
        }
    }
    return v;
}

bool selfTestAssetLicences(const fs::path& scratch, const Sha256Fn& sha256) {
    bool ok = true;
    auto expect = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::fprintf(stderr, "  self-test failed: %s\n", what.c_str());
            ok = false;
        }
    };
    auto count = [](const Violations& vs, std::string_view needle) {
        return size_t(std::count_if(vs.begin(), vs.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; }));
    };
    const fs::path good = scratch / "asset_licences_good";
    const fs::path bad = scratch / "asset_licences_bad";
    std::error_code ec;
    fs::remove_all(good, ec);
    fs::remove_all(bad, ec);

    // ---- good tree ----
    const std::string rockPng = "rock-bytes\n";
    const std::string recipe = "{\"seed\": 7}\r\n"; // CRLF checkout of an LF-hashed file still verifies
    const std::string bark = "bark";
    writeAll(good / "golden/rock030_albedo.png", rockPng);
    writeAll(good / "recipes/rock/boulder_a.json", recipe);
    writeAll(good / "golden/bark_sa_albedo.png", bark);
    writeAll(good / ".gitkeep", "");
    const std::string goodLock = std::string("{\n  \"schema\": 1,\n  \"assets\": [\n") +
        "    {\"id\": \"mat/rock/rock030_01\", \"origin\": \"sourced\", \"source\": \"ambientCG\", \"source_id\": \"Rock030\","
        " \"url\": \"https://ambientcg.com/view?id=Rock030\", \"retrieved\": \"2026-09-23\", \"retrieval\": \"direct-download\","
        " \"licence\": \"CC0-1.0\", \"author\": \"ambientCG (Lennart Demes)\", \"attribution\": null,"
        " \"files\": [{\"path\": \"golden/rock030_albedo.png\", \"sha256\": \"" + sha256(rockPng) + "\"},"
        " {\"path\": \"Rock030_2K_Color.png\", \"sha256\": \"" + std::string(64, 'a') + "\", \"location\": \"cache\"}],"
        " \"derived_from\": [], \"review\": {\"required\": false, \"by\": null, \"date\": null}},\n"
        "    {\"id\": \"dem/alps/glo30_n46e010\", \"origin\": \"sourced\", \"source\": \"Copernicus DEM GLO-30\","
        " \"url\": \"https://example.org/dem\", \"retrieved\": \"2026-09-23\", \"retrieval\": \"mirror\","
        " \"licence\": \"LicenseRef-Copernicus-DEM\", \"author\": \"DLR / Airbus\","
        " \"attribution\": \"(c) DLR e.V. 2010-2014 and (c) Airbus Defence and Space GmbH 2014-2018 | ESA\", \"files\": []},\n"
        "    {\"id\": \"mat/bark/sa_oak_01\", \"origin\": \"sourced\", \"source\": \"OpenGameArt\", \"url\": \"https://opengameart.org/x\","
        " \"retrieved\": \"2026-09-23\", \"retrieval\": \"direct-download\", \"licence\": \"CC-BY-SA-4.0\", \"author\": \"someone\","
        " \"attribution\": \"Oak bark by someone, CC-BY-SA 4.0\","
        " \"files\": [{\"path\": \"golden/bark_sa_albedo.png\", \"sha256\": \"" + sha256(bark) + "\"}],"
        " \"review\": {\"required\": true, \"by\": \"reviewer\", \"date\": \"2026-09-24\"}},\n"
        "    {\"id\": \"rock/alps/boulder_a\", \"origin\": \"generated\", \"source\": \"FUSE\", \"author\": \"FUSE contributors\","
        " \"generator\": \"Tools/FUSE/AssetGen/rock.py@0123abc\", \"seed\": 1234, \"licence\": \"LicenseRef-FUSE-Generated\","
        " \"derived_from\": [\"mat/rock/rock030_01\", \"dem/alps/glo30_n46e010\"], \"pack\": \"permissive\","
        " \"files\": [{\"path\": \"recipes/rock/boulder_a.json\", \"sha256\": \"" + sha256(foldCrlf(recipe)) + "\"}]},\n"
        "    {\"id\": \"trim/medieval/sa_trim_01\", \"origin\": \"derived\", \"source\": \"FUSE\", \"author\": \"FUSE contributors\","
        " \"licence\": \"CC-BY-SA-4.0\", \"attribution\": \"Contains oak bark by someone, CC-BY-SA 4.0\","
        " \"derived_from\": [\"mat/bark/sa_oak_01\"], \"review\": {\"required\": true, \"by\": \"reviewer\", \"date\": null}}\n"
        "  ]\n}\n";
    writeAll(good / "licences.lock.json", goodLock);
    std::string credits, err;
    expect(renderAssetCredits(good / "licences.lock.json", credits, err), "asset-licences: good lock renders CREDITS (" + err + ")");
    writeAll(good / "CREDITS.md", credits);
    writeAll(scratch / "asset_licences_cook_manifest.json",
             "{\"schemaVersion\": 1, \"assets\": [{\"kind\": \"texture\", \"sourcePath\": \"golden/rock030_albedo.png\","
             " \"outputPath\": \"cooked/rock030_albedo.fusetex\"}, {\"kind\": \"mesh\", \"sourcePath\": \"x.obj\","
             " \"outputPath\": \"cooked/boulder_a.fusemesh\", \"licence_id\": \"rock/alps/boulder_a\"}]}\n");
    const Violations vg = checkAssetLicences(good, scratch / "asset_licences_cook_manifest.json", {}, sha256);
    for (const auto& s : vg) {
        std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
    }
    expect(vg.empty(), "asset-licences: consistent lock, CRLF checkout, cache files, SA with review, credits pass");
    expect(credits.find("Copernicus") != std::string::npos && credits.find("\\|") != std::string::npos,
           "asset-licences: CREDITS lists attribution lines with '|' escaped");

    // ---- bad tree: one seeded violation per rule ----
    writeAll(bad / "golden/rock030_albedo.png", rockPng + "silently replaced");
    writeAll(bad / "golden/stray.png", "no record");
    writeAll(bad / "golden/nc.png", "nc");
    const std::string badLock = std::string("{\"schema\": 1, \"assets\": [\n") +
        // sha mismatch (rule 2)
        "{\"id\": \"mat/rock/rock030_01\", \"origin\": \"sourced\", \"source\": \"ambientCG\", \"url\": \"https://ambientcg.com/x\","
        " \"retrieved\": \"2026-09-23\", \"retrieval\": \"direct-download\", \"licence\": \"CC0-1.0\", \"author\": \"a\","
        " \"files\": [{\"path\": \"golden/rock030_albedo.png\", \"sha256\": \"" + sha256(rockPng) + "\"}]},\n"
        // forbidden NC (rule 5) + ND + editorial
        "{\"id\": \"mat/x/nc_01\", \"origin\": \"sourced\", \"source\": \"Sketchfab\", \"url\": \"https://sketchfab.com/x\","
        " \"retrieved\": \"2026-09-23\", \"retrieval\": \"api\", \"licence\": \"CC-BY-NC-4.0\", \"author\": \"a\","
        " \"files\": [{\"path\": \"golden/nc.png\", \"sha256\": \"" + sha256("nc") + "\"}]},\n"
        "{\"id\": \"mat/x/nd_01\", \"origin\": \"sourced\", \"source\": \"Sketchfab\", \"url\": \"https://sketchfab.com/y\","
        " \"retrieved\": \"2026-09-23\", \"retrieval\": \"api\", \"licence\": \"CC-BY-ND-4.0\", \"author\": \"a\"},\n"
        "{\"id\": \"prop/x/editorial_01\", \"origin\": \"sourced\", \"source\": \"Stock\", \"url\": \"https://stock.example/x\","
        " \"retrieved\": \"2026-09-23\", \"retrieval\": \"direct-download\", \"licence\": \"LicenseRef-Editorial-Only\", \"author\": \"a\"},\n"
        // share-alike and GPL art without review (rule 3)
        "{\"id\": \"mat/bark/sa_01\", \"origin\": \"sourced\", \"source\": \"OpenGameArt\", \"url\": \"https://opengameart.org/x\","
        " \"retrieved\": \"2026-09-23\", \"retrieval\": \"direct-download\", \"licence\": \"CC-BY-SA-4.0\", \"author\": \"a\","
        " \"attribution\": \"x\"},\n"
        "{\"id\": \"sprite/x/gpl_01\", \"origin\": \"sourced\", \"source\": \"OpenGameArt\", \"url\": \"https://opengameart.org/y\","
        " \"retrieved\": \"2026-09-23\", \"retrieval\": \"direct-download\", \"licence\": \"GPL-3.0-only\", \"author\": \"a\"},\n"
        // share-alike leakage into a CC0-declared derived asset (rule 4) + permissive pack
        "{\"id\": \"trim/x/leak_01\", \"origin\": \"derived\", \"source\": \"FUSE\", \"author\": \"a\", \"licence\": \"CC0-1.0\","
        " \"derived_from\": [\"mat/bark/sa_01\"], \"pack\": \"permissive\"},\n"
        // unknown provenance
        "{\"id\": \"mesh/x/mystery_01\", \"origin\": \"unknown\", \"licence\": \"NOASSERTION\"},\n"
        // research-only dataset claimed as CC0
        "{\"id\": \"anim/human/walk_01\", \"origin\": \"sourced\", \"source\": \"Bandai Namco Research Motion Dataset\","
        " \"url\": \"https://github.com/BandaiNamcoResearchInc/x\", \"retrieved\": \"2026-09-23\", \"retrieval\": \"mirror\","
        " \"licence\": \"CC0-1.0\", \"author\": \"a\"},\n"
        // generator without revision / seed (rule 7), licence not on the allow-list
        "{\"id\": \"tree/x/oak_a\", \"origin\": \"generated\", \"source\": \"FUSE\", \"author\": \"a\","
        " \"generator\": \"Tools/FUSE/AssetGen/tree.py\", \"licence\": \"WTFPL\"}\n"
        "]}\n";
    writeAll(bad / "licences.lock.json", badLock);
    writeAll(bad / "CREDITS.md", "# stale\n");
    writeAll(scratch / "asset_licences_bad_manifest.json",
             "{\"assets\": [{\"kind\": \"mesh\", \"sourcePath\": \"art/unknown.obj\", \"outputPath\": \"cooked/unknown.fusemesh\"}]}");
    const Violations vb = checkAssetLicences(bad, scratch / "asset_licences_bad_manifest.json", {}, sha256);
    for (const auto& s : vb) {
        std::fprintf(stderr, "  seeded: %s\n", s.c_str());
    }
    expect(count(vb, "sha256 mismatch") == 1u, "asset-licences: silently replaced file flagged (sha256)");
    expect(count(vb, "golden/stray.png: no licence record") == 1u, "asset-licences: file without record flagged");
    expect(count(vb, "forbidden licence") == 3u, "asset-licences: NC, ND and editorial licences flagged");
    expect(count(vb, "needs review.required") == 2u, "asset-licences: CC-BY-SA and GPL art without review flagged");
    expect(count(vb, "share-alike leakage") == 1u, "asset-licences: SA input into a CC0-declared output flagged");
    expect(count(vb, "permissive pack") == 1u, "asset-licences: SA flowing into the permissive pack flagged");
    expect(count(vb, "unknown provenance") == 1u, "asset-licences: unknown provenance reported");
    expect(count(vb, "research-only") == 1u, "asset-licences: research-only dataset flagged");
    expect(count(vb, "<path>@<git revision>") == 1u && count(vb, "numeric 'seed'") == 1u,
           "asset-licences: generator record without revision / seed flagged");
    expect(count(vb, "not on the allow-list") == 1u, "asset-licences: unlisted licence flagged");
    expect(count(vb, "CREDITS.md is missing or stale") == 1u, "asset-licences: stale CREDITS.md flagged");
    expect(count(vb, "cook manifest output") == 1u, "asset-licences: cooked output without licence record flagged");
    expect(count(vb, "requires an 'attribution'") == 0u && count(vb, ": no licence record in") == 1u,
           "asset-licences: no spurious attribution / coverage findings");
    expect(!checkAssetLicences(bad / "nonexistent", {}, {}, sha256).empty(), "asset-licences: missing lock flagged");

    fs::remove_all(good, ec);
    fs::remove_all(bad, ec);
    return ok;
}

} // namespace fuse::tools::lint
