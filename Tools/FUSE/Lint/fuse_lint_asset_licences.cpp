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

/// Rule-1 exceptions: tooling data licensed with the repository (JSON schemas, the licence policy, the fuse_ai
/// recipes: prompts, prompt context and look presets), not content.
bool isToolingDataFile(const std::string& rel) {
    return (startsWith(rel, "schemas/") && rel.ends_with(".schema.json")) || rel == "models/licence-policy.json" ||
           startsWith(rel, "recipes/ai/");
}

// ---- FUSE_REMASTER_PLAN §2.6 checks 8-12 (W0.4) -------------------------------------------------------------

/// Content/models/licence-policy.json (fuse.licence-policy/1): the §0.1.0 allow / deny lists as data.
struct LicencePolicy {
    std::set<std::string> allowCode;      ///< code_and_weights
    std::set<std::string> allowData;      ///< weights_and_data (weights / data only)
    std::set<std::string> separateProcess;///< GPL / AGPL: only as separate processes
    std::set<std::string> denyIds;
    std::vector<std::string> denyPrefixes;
    std::vector<std::string> denySubstrings; ///< lower case
    std::set<std::string> defaultProviders;
    std::set<std::string> cloudProviders;
    std::set<std::string> exceptionProviders;
    std::set<std::string> fossModels;
    std::map<std::string, std::string> nonFossModels; ///< id -> reason
    std::vector<std::string> researchData;            ///< lower case names
    std::vector<std::string> footageData;             ///< lower case names
};

const char* kPolicyRel = "models/licence-policy.json";

bool loadPolicy(const fs::path& file, LicencePolicy& p, Violations& v) {
    const std::string where = file.generic_string();
    JsonValue doc;
    std::string err;
    if (!fs::is_regular_file(file)) {
        return false;
    }
    if (!parseJson(readAll(file), doc, err)) {
        v.push_back(where + ": JSON parse error: " + err);
        return false;
    }
    if (doc["schema"].asString() != "fuse.licence-policy/1") {
        v.push_back(where + ": 'schema' must be \"fuse.licence-policy/1\"");
        return false;
    }
    auto strings = [&](const JsonValue& arr, const std::string& key, bool lower) {
        std::vector<std::string> out;
        if (!arr.isArray()) {
            v.push_back(where + ": '" + key + "' must be an array of strings");
            return out;
        }
        for (const JsonValue& s : arr.arr) {
            if (!s.isString() || s.str.empty()) {
                v.push_back(where + ": '" + key + "' holds a non-string or empty entry");
                continue;
            }
            out.push_back(lower ? lowerStr(s.str) : s.str);
        }
        return out;
    };
    auto toSet = [](const std::vector<std::string>& xs) { return std::set<std::string>(xs.begin(), xs.end()); };
    p.allowCode = toSet(strings(doc["allow"]["code_and_weights"], "allow.code_and_weights", false));
    p.allowData = toSet(strings(doc["allow"]["weights_and_data"], "allow.weights_and_data", false));
    p.separateProcess = toSet(strings(doc["allow"]["separate_process_only"], "allow.separate_process_only", false));
    p.denyIds = toSet(strings(doc["deny"]["ids"], "deny.ids", false));
    p.denyPrefixes = strings(doc["deny"]["prefixes"], "deny.prefixes", false);
    p.denySubstrings = strings(doc["deny"]["substrings_ci"], "deny.substrings_ci", true);
    p.defaultProviders = toSet(strings(doc["default_providers"], "default_providers", false));
    p.cloudProviders = toSet(strings(doc["cloud_providers"], "cloud_providers", false));
    p.fossModels = toSet(strings(doc["foss_models"], "foss_models", true));
    p.researchData = strings(doc["research_only_training_data"], "research_only_training_data", true);
    p.footageData = strings(doc["third_party_game_footage"], "third_party_game_footage", true);
    for (const JsonValue& e : doc["exceptions"].arr) {
        if (e["provider"].asString().empty() || e["reason"].asString().empty()) {
            v.push_back(where + ": every exceptions[] entry needs 'provider' and 'reason'");
        } else {
            p.exceptionProviders.insert(e["provider"].asString());
        }
    }
    for (const JsonValue& m : doc["non_foss_models"].arr) {
        const std::string id = lowerStr(m["id"].asString());
        if (id.empty() || m["reason"].asString().empty()) {
            v.push_back(where + ": every non_foss_models[] entry needs 'id' and 'reason'");
            continue;
        }
        p.nonFossModels[id] = m["reason"].asString();
        if (p.fossModels.count(id)) {
            v.push_back(where + ": model '" + id + "' is on both foss_models and non_foss_models");
        }
    }
    for (const auto& id : p.allowCode) {
        if (p.denyIds.count(id)) {
            v.push_back(where + ": licence '" + id + "' is on both the allow and the deny list");
        }
    }
    if (p.allowCode.empty() || p.denyIds.empty() || p.defaultProviders.empty()) {
        v.push_back(where + ": allow.code_and_weights, deny.ids and default_providers must not be empty");
    }
    return true;
}

enum class ModelLic { Allowed, SeparateProcess, Denied, NotListed };

ModelLic classifyModelLicence(const LicencePolicy& p, const std::string& id) {
    if (id.empty() || p.denyIds.count(id)) {
        return ModelLic::Denied;
    }
    for (const std::string& pre : p.denyPrefixes) {
        if (startsWith(id, pre)) {
            return ModelLic::Denied;
        }
    }
    const std::string low = lowerStr(id);
    for (const std::string& sub : p.denySubstrings) {
        if (low.find(sub) != std::string::npos) {
            return ModelLic::Denied;
        }
    }
    if (p.allowCode.count(id) || p.allowData.count(id)) {
        return ModelLic::Allowed;
    }
    if (p.separateProcess.count(id)) {
        return ModelLic::SeparateProcess;
    }
    return ModelLic::NotListed;
}

bool mentionsAny(const std::string& textLower, const std::vector<std::string>& names) {
    for (const std::string& n : names) {
        if (!n.empty() && textLower.find(n) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Checks 8-12 over the parsed records. `policy` is null when the policy file is missing.
void checkRemasterRules(const std::string& lockName, const std::map<std::string, Record>& recs, const fs::path& manifest,
                        const LicencePolicy* policy, const std::string& policyName, Violations& v) {
    static const std::regex shaRe(R"([0-9a-f]{64})");
    static const std::regex dateRe(R"([0-9]{4}-[0-9]{2}-[0-9]{2})");
    auto isOriginalClass = [](const Record& r) {
        const std::string origin = (*r.json)["origin"].asString();
        return startsWith(r.licence, "LicenseRef-Original-") || origin == "original" || origin == "derived-original" ||
               r.distribution == "recipe-only";
    };
    auto isOriginal = [](const Record& r) {
        const std::string origin = (*r.json)["origin"].asString();
        return startsWith(r.licence, "LicenseRef-Original-") || origin == "original" || origin == "derived-original";
    };
    // Transitive derivation closure (cycles are reported by rule 4 already).
    std::map<std::string, std::set<std::string>> chainMemo;
    std::function<const std::set<std::string>&(const std::string&, std::set<std::string>&)> chain =
        [&](const std::string& id, std::set<std::string>& stack) -> const std::set<std::string>& {
        if (auto it = chainMemo.find(id); it != chainMemo.end()) {
            return it->second;
        }
        std::set<std::string> out;
        const auto rit = recs.find(id);
        if (rit != recs.end() && stack.insert(id).second) {
            for (const JsonValue& in : (*rit->second.json)["derived_from"].arr) {
                const std::string inId = in.asString();
                if (!recs.count(inId) || out.count(inId)) {
                    continue;
                }
                out.insert(inId);
                const std::set<std::string> sub = chain(inId, stack);
                out.insert(sub.begin(), sub.end());
            }
            stack.erase(id);
        }
        return chainMemo[id] = std::move(out);
    };

    bool anyAi = false;
    for (const auto& [id, rec] : recs) {
        const JsonValue& r = *rec.json;
        const std::string where = lockName + ": asset '" + id + "'";
        std::set<std::string> stack;
        const std::set<std::string>& ancestors = chain(id, stack);

        // ---- check 8: originals / recipe-only never tracked or packed ----
        if (isOriginalClass(rec)) {
            for (const JsonValue& f : r["files"].arr) {
                if (f["location"].asString("content") == "content") {
                    v.push_back(where + ": check 8: original / recipe-only record tracks '" + f["path"].asString() +
                                "' under Content/ (originals and recipe-only derivatives live in build/remaster-cache/ only)");
                }
            }
            if (!r["pack"].asString().empty()) {
                v.push_back(where + ": check 8: original / recipe-only record is marked for pack '" + r["pack"].asString() + "'");
            }
        }
        if (!r["pack"].asString().empty() && !isOriginalClass(rec)) {
            for (const std::string& a : ancestors) {
                if (isOriginalClass(recs.at(a))) {
                    v.push_back(where + ": check 8: packed for '" + r["pack"].asString() + "' but derived from original / recipe-only '" +
                                a + "' (ship the recipe, not the output)");
                    break;
                }
            }
        }

        // ---- check 10: original in the chain + cloud uploads -> opt-in record ----
        bool originalInChain = isOriginal(rec);
        for (const std::string& a : ancestors) {
            originalInChain = originalInChain || isOriginal(recs.at(a));
        }
        const JsonValue& uploads = r["cloud_uploads"];
        if (!uploads.isNull() && !uploads.isArray()) {
            v.push_back(where + ": 'cloud_uploads' must be an array of upload-log ids");
        }
        if (originalInChain && !uploads.arr.empty()) {
            const JsonValue& opt = r["opt_in"];
            const std::string tier = r["tier"].asString(opt["tier"].asString());
            if (!opt.isObject() || opt["cloud_upload"].asString() != "opt-in" || opt["confirmed_by"].asString().empty() ||
                !std::regex_match(opt["confirmed"].asString(), dateRe)) {
                v.push_back(where + ": check 10: an original is in its derivation chain and it lists cloud_uploads, but it has no "
                                    "opt_in record (cloud_upload \"opt-in\", confirmed_by, confirmed YYYY-MM-DD; §0.1.5)");
            } else if (tier != "paid") {
                v.push_back(where + ": check 10: originals went to a cloud API on tier '" + tier + "' (only the paid tier is allowed, §0.1.5)");
            }
        }

        if (r["origin"].asString() != "ai") {
            continue;
        }
        anyAi = true;
        // ---- check 9: the recipe block ----
        const JsonValue& recipe = r["recipe"];
        const std::string modelId = lowerStr(r["model"]["id"].asString(recipe["model"].asString()));
        if (!recipe.isObject()) {
            v.push_back(where + ": check 9: ai record without a 'recipe' block (model, model_version or date, seed, output_sha256)");
        } else {
            if (recipe["model"].asString().empty()) {
                v.push_back(where + ": check 9: recipe needs the model id ('recipe.model')");
            }
            if (recipe["model_version"].asString().empty() && !std::regex_match(recipe["date"].asString(), dateRe)) {
                v.push_back(where + ": check 9: recipe needs 'model_version' or a 'date' (YYYY-MM-DD)");
            }
            if (!recipe["seed"].isNumber()) {
                v.push_back(where + ": check 9: recipe needs a numeric 'seed'");
            }
            if (!std::regex_match(recipe["output_sha256"].asString(), shaRe)) {
                v.push_back(where + ": check 9: recipe needs the 'output_sha256' (64-hex) of the reviewed output");
            }
        }
        if (!policy) {
            continue;
        }
        const std::string provider = r["provider"].asString();
        if (provider.empty()) {
            v.push_back(where + ": check 12: ai record without a 'provider'");
            continue;
        }
        const bool exempt = policy->exceptionProviders.count(provider) != 0;
        const bool mock = provider == "mock";
        // ---- check 11: model / sub-model licences, training data ----
        if (!exempt && !mock) {
            const JsonValue& m = r["model"];
            if (!m.isObject() || m["id"].asString().empty()) {
                v.push_back(where + ": check 11: ai record needs a 'model' block (id, code_licence, weights_licence, submodels, foss)");
            } else {
                bool computedFoss = true;
                auto judge = [&](const std::string& what, const std::string& lic, bool code) {
                    const ModelLic c = classifyModelLicence(*policy, lic);
                    if (c == ModelLic::Denied) {
                        v.push_back(where + ": check 11: " + what + " licence '" + (lic.empty() ? "(none)" : lic) +
                                    "' is deny-listed (§0.1.0: not FOSS, or undetermined)");
                        computedFoss = false;
                    } else if (c == ModelLic::NotListed) {
                        v.push_back(where + ": check 11: " + what + " licence '" + lic + "' is not on the §0.1.0 allow-list (" +
                                    policyName + ")");
                        computedFoss = false;
                    } else if (c == ModelLic::SeparateProcess && (!code || m["process"].asString() != "external")) {
                        v.push_back(where + ": check 11: " + what + " licence '" + lic +
                                    "' is allowed only for code run as a separate process (model.process = \"external\")");
                        computedFoss = false;
                    }
                };
                judge("model '" + m["id"].asString() + "' code", m["code_licence"].asString(), true);
                judge("model '" + m["id"].asString() + "' weights", m["weights_licence"].asString(), false);
                if (!m["submodels"].isArray()) {
                    v.push_back(where + ": check 11: model needs a 'submodels' array (empty when it loads none)");
                    computedFoss = false;
                }
                for (const JsonValue& sm : m["submodels"].arr) {
                    const std::string smId = sm["id"].asString();
                    judge("sub-model '" + smId + "'", sm["licence"].asString(), false);
                    if (const auto it = policy->nonFossModels.find(lowerStr(smId)); it != policy->nonFossModels.end()) {
                        v.push_back(where + ": check 11: sub-model '" + smId + "' is a §3.4.2 non-FOSS model (" + it->second + ")");
                        computedFoss = false;
                    }
                }
                if (const auto it = policy->nonFossModels.find(modelId); it != policy->nonFossModels.end()) {
                    v.push_back(where + ": check 11: model '" + m["id"].asString() + "' is a §3.4.2 non-FOSS model (" + it->second + ")");
                    computedFoss = false;
                }
                if (!m["foss"].isBool()) {
                    v.push_back(where + ": check 11: model needs 'foss' (true|false), computed from the §0.1.0 lists");
                } else if (m["foss"].asBool() != computedFoss) {
                    v.push_back(where + ": check 11: model says foss: " + std::string(m["foss"].asBool() ? "true" : "false") +
                                " but the §0.1.0 lists give " + (computedFoss ? "true" : "false"));
                }
                const std::string data = lowerStr(m["training_data"].asString());
                const bool reviewed = !m["training_data_review"]["by"].asString().empty();
                if (mentionsAny(data, policy->researchData) && !reviewed) {
                    v.push_back(where + ": check 11: research-only training data ('" + m["training_data"].asString() +
                                "') without a training_data_review");
                }
                if (mentionsAny(data, policy->footageData) && rec.distribution == "ship" && !reviewed) {
                    v.push_back(where + ": check 11: trained on third-party game footage ('" + m["training_data"].asString() +
                                "'): outputs default to distribution never (§4.7) unless reviewed");
                }
            }
        }
        // ---- check 12: non-default providers ----
        const bool defaultProvider = policy->defaultProviders.count(provider) != 0;
        if (!defaultProvider && !policy->fossModels.count(modelId)) {
            if (rec.distribution == "ship") {
                v.push_back(where + ": check 12: provider '" + provider + "' / model '" + modelId +
                            "' is not a default (gemini, mock or a §3.4.1 FOSS model): its output cannot ship");
            } else if (!r["non_default"].asBool()) {
                v.push_back(where + ": check 12: provider '" + provider + "' / model '" + modelId +
                            "' is non-default: flag the record with non_default: true");
            }
        }
        if (policy->cloudProviders.count(provider) && r["tier"].asString().empty()) {
            v.push_back(where + ": check 12: cloud provider '" + provider + "' needs 'tier' (paid | free)");
        }
    }
    if (anyAi && !policy) {
        v.push_back(lockName + ": ai records present but " + policyName +
                    " is missing: checks 11 and 12 need the §0.1.0 allow / deny lists");
    }

    // check 8 (cook manifest): no cooked output resolves to an original / recipe-only record.
    if (!manifest.empty() && fs::is_regular_file(manifest)) {
        JsonValue m;
        std::string perr;
        if (parseJson(readAll(manifest), m, perr)) {
            for (const JsonValue& a : m["assets"].arr) {
                const std::string lid = a["licence_id"].asString(a["licenceId"].asString());
                if (const auto it = recs.find(lid); it != recs.end() && isOriginalClass(it->second)) {
                    v.push_back(manifest.generic_string() + ": check 8: cooked output '" + a["outputPath"].asString(a["output_path"].asString()) +
                                "' comes from original / recipe-only record '" + lid + "' (reachable from a pack)");
                }
            }
        }
    }
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
    // FUSE_REMASTER_PLAN §2.6 checks 8-12 (W0.4), with the §0.1.0 lists from models/licence-policy.json.
    {
        const fs::path policyFile = content / kPolicyRel;
        LicencePolicy policy;
        const bool havePolicy = loadPolicy(policyFile, policy, v);
        checkRemasterRules(lockName, recs, manifest, havePolicy ? &policy : nullptr, policyFile.generic_string(), v);
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
        if (isRootManifestFile(relp) || isToolingDataFile(relp) || name == ".gitkeep" || name == ".gitignore") {
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

bool selfTestAssetLicences(const fs::path& scratch, const Sha256Fn& sha256, const fs::path& policyFile) {
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

    // ---- FUSE_REMASTER_PLAN §2.6 checks 8-12: a clean tree and one seeded failure (or more) per check ----
    {
        const fs::path rg = scratch / "asset_licences_remaster_good";
        const fs::path rb = scratch / "asset_licences_remaster_bad";
        fs::remove_all(rg, ec);
        fs::remove_all(rb, ec);
        // The embedded policy: the §0.1.0 entries these trees use (the real Content/models/licence-policy.json must
        // judge them the same way; checked below when the caller passes it).
        const std::string miniPolicy =
            "{\"schema\": \"fuse.licence-policy/1\","
            " \"allow\": {\"code_and_weights\": [\"Apache-2.0\", \"MIT\", \"BSD-3-Clause\"], \"weights_and_data\": [\"CC0-1.0\"],"
            " \"separate_process_only\": [\"GPL-3.0-only\"]},"
            " \"deny\": {\"ids\": [\"LicenseRef-DINOv3\", \"LicenseRef-Bria-RMBG\", \"LicenseRef-NVIDIA-Source-Code\", \"NOASSERTION\"],"
            " \"prefixes\": [\"CC-BY-NC\", \"CC-BY-ND\", \"LicenseRef-Tencent-\"], \"substrings_ci\": [\"-nc\", \"-nd\", \"community\", \"research-only\"]},"
            " \"default_providers\": [\"gemini\", \"mock\"], \"cloud_providers\": [\"gemini\"],"
            " \"exceptions\": [{\"provider\": \"gemini\", \"reason\": \"user-approved agent\"}],"
            " \"foss_models\": [\"pbrify_remix\", \"triposr\", \"birefnet\", \"demucs-v4\"],"
            " \"non_foss_models\": [{\"id\": \"pbrfusion\", \"reason\": \"CC-BY-NC-SA-4.0\"}, {\"id\": \"dinov3\", \"reason\": \"DINOv3 licence\"},"
            " {\"id\": \"trellis.2-4b\", \"reason\": \"sub-model rule\"}],"
            " \"research_only_training_data\": [\"DIV2K\", \"MUSDB18\", \"Objaverse\"], \"third_party_game_footage\": [\"GTA V\", \"Minecraft\"]}\n";
        const std::string sha0 = std::string(64, '0');
        const std::string recipeOk = "\"recipe\": {\"path\": \"recipes/x.json\", \"model\": \"m\", \"date\": \"2026-11-05\", \"seed\": 7,"
                                     " \"output_sha256\": \"" + std::string(64, 'b') + "\"}";
        const std::string signedReview = "\"review\": {\"required\": true, \"by\": \"reviewer\", \"date\": \"2026-11-05\"}";
        const std::string gen = "\"generator\": \"Tools/FUSE/Remaster/fuse_ai@0123abc\", \"seed\": 7";
        auto original = [&](const std::string& id, const std::string& extra) {
            return "{\"id\": \"" + id + "\", \"origin\": \"original\", \"source\": \"user-owned game g\", \"author\": \"game authors\","
                   " \"licence\": \"LicenseRef-Original-g\", \"distribution\": \"never\", \"game_id\": \"g\", " + signedReview +
                   ", \"files\": [{\"path\": \"g/" + id.substr(id.rfind('/') + 1) + ".dds\", \"sha256\": \"" + sha0 +
                   "\", \"location\": \"cache\"}]" + extra + "}";
        };
        auto ai = [&](const std::string& id, const std::string& body) {
            return "{\"id\": \"" + id + "\", \"origin\": \"ai\", \"source\": \"FUSE\", \"author\": \"FUSE contributors\", " + gen + ", " +
                   body + "}";
        };
        const std::string blockout = "blockout";
        writeAll(rg / "golden/rock_blockout.glb", blockout);
        const std::string goodRm = std::string("{\"schema\": 1, \"assets\": [\n") + original("orig/g/wall_tex", "") + ",\n" +
            // a local FOSS upscale of an original: recipe-only, its output stays in the cache
            ai("ai/g/wall_tex_up", "\"provider\": \"local-onnx\", \"licence\": \"LicenseRef-Original-g\", \"distribution\": \"recipe-only\", " +
                                       signedReview + ", \"derived_from\": [\"orig/g/wall_tex\"], \"model\": {\"id\": \"pbrify_remix\","
                                       " \"code_licence\": \"MIT\", \"weights_licence\": \"CC0-1.0\", \"submodels\": [], \"foss\": true,"
                                       " \"training_data\": \"ambientCG CC0\"}, " + recipeOk) + ",\n" +
            // Gemini tags of an original, uploaded with the per-game opt-in on the paid tier
            ai("ai/g/door_tags", "\"provider\": \"gemini\", \"tier\": \"paid\", \"licence\": \"LicenseRef-AI-Gemini\", \"distribution\": \"never\", " +
                                     signedReview + ", \"derived_from\": [\"orig/g/wall_tex\"], \"model\": {\"id\": \"gemini-3.8-flash\"},"
                                     " \"cloud_uploads\": [\"upload-0001\"], \"opt_in\": {\"cloud_upload\": \"opt-in\", \"confirmed_by\": \"owner\","
                                     " \"confirmed\": \"2026-11-01\"}, " + recipeOk) + ",\n" +
            // a shippable blockout from our own prompt: FOSS model + FOSS sub-model, reviewed research-ish training data
            ai("ai/lib/rock_blockout", "\"provider\": \"local-torch\", \"licence\": \"LicenseRef-FUSE-Generated\", \"pack\": \"permissive\","
                                           " \"model\": {\"id\": \"TripoSR\", \"code_licence\": \"MIT\", \"weights_licence\": \"MIT\","
                                           " \"submodels\": [{\"id\": \"birefnet\", \"licence\": \"MIT\"}], \"foss\": true,"
                                           " \"training_data\": \"Objaverse renders\", \"training_data_review\": {\"by\": \"reviewer\", \"date\": \"2026-11-05\"}},"
                                           " \"files\": [{\"path\": \"golden/rock_blockout.glb\", \"sha256\": \"" + sha256(blockout) + "\"}], " + recipeOk) + ",\n" +
            ai("ai/lib/mock_tags", "\"provider\": \"mock\", \"licence\": \"LicenseRef-FUSE-Generated\", \"model\": {\"id\": \"mock-tagger\"}, " + recipeOk) + ",\n" +
            // a non-default provider, flagged and not shipped
            ai("ai/lib/comfy_variant", "\"provider\": \"comfyui\", \"licence\": \"LicenseRef-FUSE-Generated\", \"distribution\": \"never\","
                                           " \"non_default\": true, \"model\": {\"id\": \"own-upscaler\", \"code_licence\": \"GPL-3.0-only\","
                                           " \"process\": \"external\", \"weights_licence\": \"CC0-1.0\", \"submodels\": [], \"foss\": true}, " + recipeOk) +
            "\n]}\n";
        writeAll(rg / "licences.lock.json", goodRm);
        writeAll(rg / "models/licence-policy.json", miniPolicy);
        std::string rcredits;
        expect(renderAssetCredits(rg / "licences.lock.json", rcredits, err), "remaster checks: good lock renders CREDITS (" + err + ")");
        writeAll(rg / "CREDITS.md", rcredits);
        const Violations vrg = checkAssetLicences(rg, {}, {}, sha256);
        for (const auto& s : vrg) {
            std::fprintf(stderr, "  unexpected (remaster good): %s\n", s.c_str());
        }
        expect(vrg.empty(), "remaster checks 8-12: originals in the cache, opt-in Gemini upload, FOSS model + sub-model, mock, "
                            "flagged non-default provider pass");

        writeAll(rb / "remaster/leak.dds", "original pixels");
        const std::string badRm = std::string("{\"schema\": 1, \"assets\": [\n") + original("orig/g/wall_tex", "") + ",\n" +
            // check 8: an original tracked under Content/, a recipe-only record packed, a packed derivative of an original
            "{\"id\": \"orig/g/leak\", \"origin\": \"original\", \"source\": \"g\", \"author\": \"a\", \"licence\": \"LicenseRef-Original-g\","
            " \"distribution\": \"never\", " + signedReview + ", \"files\": [{\"path\": \"remaster/leak.dds\", \"sha256\": \"" +
            sha256("original pixels") + "\"}]},\n" +
            "{\"id\": \"der/g/packed_up\", \"origin\": \"derived-original\", \"source\": \"FUSE\", \"author\": \"a\", \"licence\": \"LicenseRef-Original-g\","
            " \"distribution\": \"recipe-only\", \"pack\": \"remaster-g\", " + signedReview + ", \"derived_from\": [\"orig/g/wall_tex\"]},\n" +
            "{\"id\": \"lib/x/packed_child\", \"origin\": \"generated\", \"source\": \"FUSE\", \"author\": \"a\", " + gen +
            ", \"licence\": \"LicenseRef-FUSE-Generated\", \"distribution\": \"never\", \"pack\": \"remaster-g\", \"derived_from\": [\"der/g/packed_up\"]},\n" +
            // check 9: no recipe; recipe without seed and output hash
            ai("ai/x/no_recipe", "\"provider\": \"mock\", \"licence\": \"LicenseRef-FUSE-Generated\", \"model\": {\"id\": \"mock-tagger\"}") + ",\n" +
            ai("ai/x/bad_recipe", "\"provider\": \"mock\", \"licence\": \"LicenseRef-FUSE-Generated\", \"model\": {\"id\": \"mock-tagger\"},"
                                  " \"recipe\": {\"model\": \"mock-tagger\", \"model_version\": \"1\"}") + ",\n" +
            // check 10: original uploaded without an opt-in record; with an opt-in on the free tier
            ai("ai/g/no_opt_in", "\"provider\": \"gemini\", \"tier\": \"paid\", \"licence\": \"LicenseRef-AI-Gemini\", \"distribution\": \"never\", " +
                                     signedReview + ", \"derived_from\": [\"orig/g/wall_tex\"], \"model\": {\"id\": \"gemini-3.8-flash\"},"
                                     " \"cloud_uploads\": [\"upload-0002\"], " + recipeOk) + ",\n" +
            ai("ai/g/free_tier", "\"provider\": \"gemini\", \"tier\": \"free\", \"licence\": \"LicenseRef-AI-Gemini\", \"distribution\": \"never\", " +
                                     signedReview + ", \"derived_from\": [\"orig/g/wall_tex\"], \"model\": {\"id\": \"gemini-3.8-flash\"},"
                                     " \"cloud_uploads\": [\"upload-0003\"], \"opt_in\": {\"cloud_upload\": \"opt-in\", \"confirmed_by\": \"owner\","
                                     " \"confirmed\": \"2026-11-01\"}, " + recipeOk) + ",\n" +
            // check 11: NC weights (PBRFusion), a deny-listed sub-model (DINOv3 inside a TRELLIS.2-like pipeline),
            // research-only training data without review, GPL code linked in-process
            ai("ai/x/pbrfusion", "\"provider\": \"local-onnx\", \"licence\": \"LicenseRef-FUSE-Generated\", \"distribution\": \"never\","
                                 " \"non_default\": true, \"model\": {\"id\": \"PBRFusion\", \"code_licence\": \"MIT\", \"weights_licence\": \"CC-BY-NC-SA-4.0\","
                                 " \"submodels\": [], \"foss\": true}, " + recipeOk) + ",\n" +
            ai("ai/x/submodel", "\"provider\": \"local-torch\", \"licence\": \"LicenseRef-FUSE-Generated\", \"distribution\": \"never\","
                                " \"non_default\": true, \"model\": {\"id\": \"pipeline-3d\", \"code_licence\": \"MIT\", \"weights_licence\": \"MIT\","
                                " \"submodels\": [{\"id\": \"DINOv3\", \"licence\": \"LicenseRef-DINOv3\"}], \"foss\": false}, " + recipeOk) + ",\n" +
            ai("ai/x/div2k", "\"provider\": \"local-onnx\", \"licence\": \"LicenseRef-FUSE-Generated\", \"distribution\": \"never\","
                             " \"model\": {\"id\": \"demucs-v4\", \"code_licence\": \"MIT\", \"weights_licence\": \"MIT\", \"submodels\": [],"
                             " \"foss\": true, \"training_data\": \"DIV2K (academic only)\"}, " + recipeOk) + ",\n" +
            ai("ai/x/gpl_linked", "\"provider\": \"local-torch\", \"licence\": \"LicenseRef-FUSE-Generated\", \"distribution\": \"never\","
                                  " \"model\": {\"id\": \"triposr\", \"code_licence\": \"GPL-3.0-only\", \"weights_licence\": \"MIT\", \"submodels\": [],"
                                  " \"foss\": false}, " + recipeOk) + ",\n" +
            // check 12: a non-default provider / model whose output ships; one that is not flagged
            ai("ai/x/ships", "\"provider\": \"comfyui\", \"licence\": \"LicenseRef-FUSE-Generated\", \"model\": {\"id\": \"mystery-upscaler\","
                             " \"code_licence\": \"MIT\", \"weights_licence\": \"MIT\", \"submodels\": [], \"foss\": true}, " + recipeOk) + ",\n" +
            ai("ai/x/unflagged", "\"provider\": \"comfyui\", \"licence\": \"LicenseRef-FUSE-Generated\", \"distribution\": \"never\","
                                 " \"model\": {\"id\": \"mystery-upscaler\", \"code_licence\": \"MIT\", \"weights_licence\": \"MIT\", \"submodels\": [],"
                                 " \"foss\": true}, " + recipeOk) +
            "\n]}\n";
        writeAll(rb / "licences.lock.json", badRm);
        writeAll(rb / "models/licence-policy.json", miniPolicy);
        std::string bcredits;
        expect(renderAssetCredits(rb / "licences.lock.json", bcredits, err), "remaster checks: bad lock renders CREDITS (" + err + ")");
        writeAll(rb / "CREDITS.md", bcredits);
        writeAll(scratch / "asset_licences_remaster_manifest.json",
                 "{\"assets\": [{\"kind\": \"texture\", \"sourcePath\": \"x.png\", \"outputPath\": \"cooked/up.fusetex\", \"licence_id\": \"der/g/packed_up\"}]}");
        auto checkLines = [](const Violations& vs) {
            Violations out;
            for (const auto& s : vs) {
                if (s.find(": check ") != std::string::npos) {
                    // The policies' free-text reasons differ; the finding itself must not.
                    std::string t = s;
                    if (const size_t at = t.find("non-FOSS model ("); at != std::string::npos) {
                        t = t.substr(0, at + 14);
                    }
                    out.push_back(t);
                }
            }
            return out;
        };
        const Violations vrb = checkAssetLicences(rb, scratch / "asset_licences_remaster_manifest.json", {}, sha256);
        const Violations crb = checkLines(vrb);
        for (const auto& s : crb) {
            std::fprintf(stderr, "  seeded: %s\n", s.c_str());
        }
        expect(count(crb, "check 8: original / recipe-only record tracks 'remaster/leak.dds'") == 1u, "check 8: original tracked under Content/ flagged");
        expect(count(crb, "check 8: original / recipe-only record is marked for pack") == 1u, "check 8: packed recipe-only record flagged");
        expect(count(crb, "check 8: packed for 'remaster-g' but derived from original") == 1u, "check 8: packed derivative of an original flagged");
        expect(count(crb, "check 8: cooked output 'cooked/up.fusetex'") == 1u, "check 8: cooked output of a recipe-only record flagged");
        expect(count(crb, "check 9: ai record without a 'recipe'") == 1u, "check 9: ai record without a recipe flagged");
        expect(count(crb, "check 9: recipe needs a numeric 'seed'") == 1u && count(crb, "check 9: recipe needs the 'output_sha256'") == 1u,
               "check 9: recipe without seed / output hash flagged");
        expect(count(crb, "check 10: an original is in its derivation chain") == 1u, "check 10: upload of an original without opt-in flagged");
        expect(count(crb, "check 10: originals went to a cloud API on tier 'free'") == 1u, "check 10: opt-in on the free tier flagged");
        expect(count(crb, "check 11: model 'PBRFusion' weights licence 'CC-BY-NC-SA-4.0' is deny-listed") == 1u &&
                   count(crb, "check 11: model 'PBRFusion' is a §3.4.2 non-FOSS model") == 1u &&
                   count(crb, "says foss: true but the §0.1.0 lists give false") == 1u,
               "check 11: NC weights (PBRFusion) flagged, also as a non-FOSS model with a wrong foss flag");
        expect(count(crb, "check 11: sub-model 'DINOv3' licence 'LicenseRef-DINOv3' is deny-listed") == 1u &&
                   count(crb, "check 11: sub-model 'DINOv3' is a §3.4.2 non-FOSS model") == 1u,
               "check 11: deny-listed sub-model (DINOv3) flagged");
        expect(count(crb, "check 11: research-only training data ('DIV2K") == 1u, "check 11: research-only training data without review flagged");
        expect(count(crb, "check 11: model 'triposr' code licence 'GPL-3.0-only' is allowed only for code run as a separate process") == 1u,
               "check 11: GPL code linked in-process flagged");
        expect(count(crb, "check 12: provider 'comfyui' / model 'mystery-upscaler' is not a default") == 1u,
               "check 12: shippable output of a non-default provider flagged");
        expect(count(crb, "check 12: provider 'comfyui' / model 'mystery-upscaler' is non-default: flag") == 1u,
               "check 12: unflagged non-default provider flagged");
        expect(crb.size() == 18u, "remaster checks: exactly the seeded findings (got " + std::to_string(crb.size()) + ")");
        // Missing policy with ai records is itself a finding; a broken policy is reported.
        fs::remove(rb / "models/licence-policy.json", ec);
        expect(count(checkAssetLicences(rb, {}, {}, sha256), "is missing: checks 11 and 12 need") == 1u, "missing licence policy flagged");
        writeAll(rb / "models/licence-policy.json", "{\"schema\": \"fuse.licence-policy/1\", \"allow\": {\"code_and_weights\": [\"MIT\"]},"
                                                    " \"deny\": {\"ids\": [\"MIT\"]}, \"default_providers\": [\"mock\"]}");
        expect(count(checkAssetLicences(rb, {}, {}, sha256), "is on both the allow and the deny list") == 1u, "contradictory policy flagged");
        // The real policy (Content/models/licence-policy.json) judges both trees exactly like the embedded one.
        if (!policyFile.empty() && fs::is_regular_file(policyFile)) {
            const std::string real = readAll(policyFile);
            writeAll(rb / "models/licence-policy.json", real);
            writeAll(rg / "models/licence-policy.json", real);
            const Violations realBad = checkLines(checkAssetLicences(rb, scratch / "asset_licences_remaster_manifest.json", {}, sha256));
            const Violations realGood = checkAssetLicences(rg, {}, {}, sha256);
            for (const auto& s : realGood) {
                std::fprintf(stderr, "  unexpected (real policy, good): %s\n", s.c_str());
            }
            expect(realGood.empty(), "remaster checks with " + policyFile.generic_string() + ": the clean tree passes");
            expect(realBad == crb, "remaster checks with " + policyFile.generic_string() +
                                       ": the same 18 findings as the embedded policy (deny-listed sub-model included)");
            if (realBad != crb) {
                for (const auto& s : realBad) {
                    std::fprintf(stderr, "  real policy: %s\n", s.c_str());
                }
            }
        }
        fs::remove_all(rg, ec);
        fs::remove_all(rb, ec);
        fs::remove(scratch / "asset_licences_remaster_manifest.json", ec);
    }

    fs::remove_all(good, ec);
    fs::remove_all(bad, ec);
    return ok;
}

} // namespace fuse::tools::lint
