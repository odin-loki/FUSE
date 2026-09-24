// FUSE Relight RL-3.1: fuse_relight_usd_tool, the USD reader's command line.
//
//   dump <root layer> [--no-diagnostics]       canonical flattened JSON (see canonical.cpp) to stdout
//   remix <root layer>                         the Remix view (replacements, categories, particles)
//   layer <layer>                              one layer's specs as loaded (no composition; debugging)
//   to-usdc <in layer> <out.usdc>              re-encode one layer with TinyUSDZ's crate writer
//   usdc-fixture <src dir> <dst dir>           a fixture's USDC twin: every *.usd layer re-encoded under the same
//                                              name, *.usda -> *.usdc (only root layers may be .usda), other
//                                              files copied. Deterministic (the fixture USDC files are generated
//                                              at test time, never committed).
//   check <root layer> <expect file> [--same-as <other root>]
//                                              evaluates a fixture's expectations; --same-as additionally requires
//                                              the canonical dump (diagnostic layers compared by stem) to equal
//                                              the other root's (the USDA original of a USDC twin). In the expect
//                                              file, "[usda] ..." lines apply to .usda roots only and
//                                              "same-as-ignore-diagnostic <code>" drops that code from the
//                                              --same-as comparison: both mark what TinyUSDZ's crate writer cannot
//                                              carry into a USDC twin.
// Exit codes: 0 ok, 1 check failed, 2 usage / I/O error.
#include <fuse/relight/mods/usd/remix_profile.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <fuse/relight/hash/hash_string.hpp>

#include "crate-writer.hh"
#include "tinyusdz.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
namespace usd = fuse::relight::mods::usd;

namespace {

std::optional<std::string> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeFile(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::string generic(const fs::path& p) { return p.generic_string(); }

// ---- to-usdc ------------------------------------------------------------------------------------------------

bool toUsdc(const fs::path& in, const fs::path& out, std::string& err) {
    const auto bytes = readFile(in);
    if (!bytes) {
        err = "cannot read " + generic(in);
        return false;
    }
    tinyusdz::Layer layer;
    std::string warn;
    tinyusdz::USDLoadOptions opts;
    opts.load_assets = false;
    std::string text = *bytes;
    const auto offsets = usd::stripSubLayerOffsets(text); // TinyUSDZ's USDA parser cannot read them
    if (!tinyusdz::LoadLayerFromMemory(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), generic(in), &layer,
                                       &warn, &err, opts)) {
        return false;
    }
    for (std::size_t i = 0; i < offsets.size() && i < layer.metas().subLayers.size(); ++i) {
        layer.metas().subLayers[i].layerOffset._offset = offsets[i].first;
        layer.metas().subLayers[i].layerOffset._scale = offsets[i].second;
    }
    auto stream = std::make_unique<tinyusdz::experimental::MemoryOutputStream>();
    auto* mem = stream.get();
    tinyusdz::experimental::CrateWriter writer(std::unique_ptr<tinyusdz::experimental::IOutputStream>(std::move(stream)));
    tinyusdz::experimental::CrateWriter::Options wopts;
    wopts.version_major = 0;
    wopts.version_minor = 8;
    wopts.version_patch = 0;
    writer.SetOptions(wopts);
    if (!writer.Open(&err) || !writer.ConvertLayerToSpecs(layer, &err) || !writer.Finalize(&err)) {
        err = "crate writer: " + err;
        return false;
    }
    writer.Close();
    const std::vector<std::uint8_t> buf = mem->TakeBuffer();
    return writeFile(out, std::string(buf.begin(), buf.end())) || ((err = "cannot write " + generic(out)), false);
}

int usdcFixture(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(src)) {
        if (e.is_regular_file()) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        const fs::path rel = fs::relative(f, src);
        fs::path target = dst / rel;
        fs::create_directories(target.parent_path(), ec);
        const std::string ext = f.extension().string();
        if (ext == ".usd" || ext == ".usda") {
            if (ext == ".usda") {
                target.replace_extension(".usdc");
            }
            std::string err;
            if (!toUsdc(f, target, err)) {
                std::cerr << "usdc-fixture: " << generic(rel) << ": " << err << "\n";
                return 2;
            }
        } else {
            fs::copy_file(f, target, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "usdc-fixture: cannot copy " << generic(rel) << "\n";
                return 2;
            }
        }
    }
    return 0;
}

// ---- remix --------------------------------------------------------------------------------------------------

void printRemix(const usd::RemixMod& mod) {
    namespace hash = fuse::relight::hash;
    for (const auto& m : mod.materials) {
        std::cout << "material " << hash::hashToString(m.hash) << " " << m.path << (m.hashFromName ? "" : " (origin hash)")
                  << " shader=" << m.shaderPath << " mdl=" << m.mdlSourceAsset << (m.particles ? " particles" : "") << "\n";
    }
    for (const auto& m : mod.meshes) {
        std::cout << "mesh " << hash::hashToString(m.hash) << " " << m.path << " meshes=" << m.meshPrims.size()
                  << " lights=" << m.lightPrims.size() << " categories=" << m.categories.flags.toString()
                  << (m.particles ? " particles" : "") << "\n";
    }
    for (const auto& l : mod.lights) {
        std::cout << "light " << hash::hashToString(l.hash) << " " << l.path << " " << l.type << "\n";
    }
    for (const auto& d : mod.diagnostics) {
        std::cout << "diagnostic " << d.code << " " << d.primPath << "\n";
    }
}

// ---- check --------------------------------------------------------------------------------------------------

bool nearlyEqual(const usd::Value& a, const usd::Value& b) {
    if (a.kind == usd::Value::Kind::Number && b.kind == usd::Value::Kind::Number) {
        if (std::isnan(a.number) || std::isnan(b.number)) {
            return std::isnan(a.number) && std::isnan(b.number);
        }
        if (a.number == b.number) {
            return true; // also +-inf
        }
        const double tol = 1e-6 * std::max({1.0, std::fabs(a.number), std::fabs(b.number)});
        return std::fabs(a.number - b.number) <= tol;
    }
    // Bools authored as 0 / 1 in USDA and printed as true / false (or the reverse) compare equal.
    if ((a.kind == usd::Value::Kind::Bool || b.kind == usd::Value::Kind::Bool) && a.asBool() && b.asBool() &&
        (a.kind == usd::Value::Kind::Bool) != (b.kind == usd::Value::Kind::Bool)) {
        return *a.asBool() == *b.asBool();
    }
    if (a.kind != b.kind || a.items.size() != b.items.size() || a.dict.size() != b.dict.size()) {
        return false;
    }
    if (a.kind == usd::Value::Kind::Bool) {
        return a.boolean == b.boolean;
    }
    if (a.kind == usd::Value::Kind::String || a.kind == usd::Value::Kind::Asset || a.kind == usd::Value::Kind::Path) {
        if (a.text != b.text) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        if (!nearlyEqual(a.items[i], b.items[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.dict.size(); ++i) {
        if (a.dict[i].first != b.dict[i].first || !nearlyEqual(a.dict[i].second, b.dict[i].second)) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string w;
    while (in >> w) {
        out.push_back(w);
    }
    return out;
}

/// "<prim path>.<property>" -> (prim, property); the property starts after the first '.' following the last '/'.
std::pair<std::string, std::string> splitProp(const std::string& s) {
    const std::size_t slash = s.rfind('/');
    const std::size_t dot = s.find('.', slash == std::string::npos ? 0 : slash);
    if (dot == std::string::npos) {
        return {s, ""};
    }
    return {s.substr(0, dot), s.substr(dot + 1)};
}

std::string stem(const std::string& layer) {
    const fs::path p(layer);
    return generic(p.parent_path() / p.stem());
}

/// Canonical dump with the diagnostic layer names reduced to their stems (".usda" / ".usdc" twins compare equal).
std::string comparableDump(const usd::ComposedStage& s, const std::vector<std::string>& ignoredCodes) {
    usd::ComposedStage copy = s;
    std::erase_if(copy.diagnostics, [&](const usd::Diagnostic& d) {
        return std::find(ignoredCodes.begin(), ignoredCodes.end(), d.code) != ignoredCodes.end();
    });
    for (usd::Diagnostic& d : copy.diagnostics) {
        d.layer = stem(d.layer);
    }
    return usd::canonicalDump(copy, true);
}

class Checker {
public:
    Checker(const usd::ComposedStage& stage, const usd::RemixMod& mod, bool usdc) : m_stage(stage), m_mod(mod), m_usdc(usdc) {}
    int failures = 0;
    int checks = 0;
    std::vector<std::string> sameAsIgnoredDiagnostics; ///< `same-as-ignore-diagnostic <code>` directives

    void fail(int line, const std::string& what) {
        ++failures;
        std::cerr << "expect:" << line << ": " << what << "\n";
    }

    const usd::Prim* prim(int line, const std::string& path) {
        const usd::Prim* p = m_stage.find(path);
        if (!p) {
            fail(line, "no prim " + path);
        }
        return p;
    }

    std::optional<usd::Value> value(int line, const std::string& text) {
        std::string err;
        auto v = usd::parseValue(text, &err);
        if (!v) {
            fail(line, "bad expected value '" + text + "': " + err);
        }
        return v;
    }

    const usd::Attribute* attr(int line, const std::string& ref) {
        const auto [path, name] = splitProp(ref);
        const usd::Prim* p = prim(line, path);
        if (!p) {
            return nullptr;
        }
        const usd::Attribute* a = p->attribute(name);
        if (!a) {
            fail(line, "no attribute " + ref);
        }
        return a;
    }

    void run(const std::string& text) {
        std::istringstream in(text);
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::size_t hash = line.find(" #");
            if (hash != std::string::npos) {
                line.resize(hash);
            }
            auto w = words(line);
            if (w.empty() || w[0][0] == '#') {
                continue;
            }
            // "[usda] <expectation>": only for USDA roots (a fact the TinyUSDZ crate writer cannot carry into the
            // generated USDC twin; the comment next to it says which).
            if (w[0] == "[usda]") {
                if (m_usdc) {
                    continue;
                }
                line = line.substr(line.find("[usda]") + 6);
                w.erase(w.begin());
                if (w.empty()) {
                    continue;
                }
            }
            if (w[0] == "same-as-ignore-diagnostic" && w.size() == 2) {
                sameAsIgnoredDiagnostics.push_back(w[1]);
                continue;
            }
            ++checks;
            one(n, line, w);
        }
    }

private:
    std::string rhs(const std::string& line) {
        const std::size_t eq = line.find(" = ");
        return eq == std::string::npos ? "" : line.substr(eq + 3);
    }

    void one(int n, const std::string& line, const std::vector<std::string>& w) {
        const std::string& op = w[0];
        if (op == "prim" && w.size() >= 2) {
            const usd::Prim* p = prim(n, w[1]);
            if (!p) {
                return;
            }
            for (std::size_t i = 2; i < w.size(); ++i) {
                const std::size_t eq = w[i].find('=');
                const std::string k = w[i].substr(0, eq), v = eq == std::string::npos ? "" : w[i].substr(eq + 1);
                std::string actual;
                if (k == "type") actual = p->typeName;
                else if (k == "specifier") actual = usd::specifierName(p->specifier);
                else if (k == "active") actual = p->active ? "true" : "false";
                else if (k == "instanceable") actual = p->instanceable ? "true" : "false";
                else if (k == "kind") actual = p->kind;
                else {
                    fail(n, "unknown prim field " + k);
                    continue;
                }
                if (actual != v) {
                    fail(n, w[1] + " " + k + " is '" + actual + "', expected '" + v + "'");
                }
            }
        } else if (op == "noprim" && w.size() == 2) {
            if (m_stage.find(w[1])) {
                fail(n, "unexpected prim " + w[1]);
            }
        } else if (op == "prims" && w.size() == 2) {
            if (std::to_string(m_stage.prims.size()) != w[1]) {
                fail(n, "stage has " + std::to_string(m_stage.prims.size()) + " prims, expected " + w[1]);
            }
        } else if (op == "attr" && w.size() >= 3) {
            const usd::Attribute* a = attr(n, w[1]);
            const auto v = value(n, rhs(line));
            if (a && v) {
                if (!a->hasDefault) {
                    fail(n, w[1] + " has no default value");
                } else if (!nearlyEqual(a->defaultValue, *v)) {
                    fail(n, w[1] + " = " + usd::formatValue(a->defaultValue) + ", expected " + usd::formatValue(*v));
                }
            }
        } else if (op == "noattr" && w.size() == 2) {
            const auto [path, name] = splitProp(w[1]);
            if (const usd::Prim* p = prim(n, path); p && p->attribute(name)) {
                fail(n, "unexpected attribute " + w[1]);
            }
        } else if (op == "novalue" && w.size() == 2) {
            if (const usd::Attribute* a = attr(n, w[1]); a && a->hasDefault && !a->defaultValue->isNone()) {
                fail(n, w[1] + " has value " + usd::formatValue(a->defaultValue));
            }
        } else if (op == "attrtype" && w.size() == 3) {
            if (const usd::Attribute* a = attr(n, w[1]); a && a->typeName != w[2]) {
                fail(n, w[1] + " type is " + a->typeName + ", expected " + w[2]);
            }
        } else if (op == "uniform" && w.size() == 3) {
            if (const usd::Attribute* a = attr(n, w[1]); a && (a->uniform ? "true" : "false") != w[2]) {
                fail(n, w[1] + " uniform mismatch");
            }
        } else if (op == "attrmeta" && w.size() >= 4) {
            const usd::Attribute* a = attr(n, w[1]);
            const auto v = value(n, rhs(line));
            if (a && v) {
                const auto it = std::find_if(a->metadata.begin(), a->metadata.end(), [&](const auto& e) { return e.first == w[2]; });
                if (it == a->metadata.end()) {
                    fail(n, w[1] + " has no metadata " + w[2]);
                } else if (!nearlyEqual(it->second, *v)) {
                    fail(n, w[1] + " metadata " + w[2] + " = " + usd::formatValue(it->second));
                }
            }
        } else if (op == "resolved" && w.size() == 3) {
            if (const usd::Attribute* a = attr(n, w[1])) {
                const std::string base = usd::parentDir(m_stage.rootLayer);
                std::string r = a->defaultValue->resolved;
                if (r.compare(0, base.size() + 1, base + "/") == 0) {
                    r = r.substr(base.size() + 1);
                }
                if (a->defaultValue->kind != usd::Value::Kind::Asset || r != w[2]) {
                    fail(n, w[1] + " resolves to '" + r + "', expected '" + w[2] + "'");
                }
            }
        } else if (op == "samples" && w.size() >= 3) {
            const usd::Attribute* a = attr(n, w[1]);
            const auto v = value(n, rhs(line));
            if (a && v) {
                bool ok = v->kind == usd::Value::Kind::Dict && v->dict.size() == a->timeSamples.size();
                for (std::size_t i = 0; ok && i < v->dict.size(); ++i) {
                    ok = std::fabs(std::strtod(v->dict[i].first.c_str(), nullptr) - a->timeSamples[i].first) < 1e-9 &&
                         nearlyEqual(v->dict[i].second, a->timeSamples[i].second);
                }
                if (!ok) {
                    std::string got = "{";
                    for (const auto& [t, s] : a->timeSamples) {
                        got += " " + usd::formatNumber(t) + ": " + usd::formatValue(s) + ",";
                    }
                    fail(n, w[1] + " time samples " + got + " }");
                }
            }
        } else if ((op == "rel" || op == "conn") && w.size() >= 3) {
            const auto [path, name] = splitProp(w[1]);
            const usd::Prim* p = prim(n, path);
            const auto v = value(n, rhs(line));
            if (!p || !v) {
                return;
            }
            std::vector<std::string> actual;
            if (op == "rel") {
                const usd::Relationship* r = p->relationship(name);
                if (!r) {
                    fail(n, "no relationship " + w[1]);
                    return;
                }
                actual = r->targets;
            } else {
                const usd::Attribute* a = p->attribute(name);
                if (!a) {
                    fail(n, "no attribute " + w[1]);
                    return;
                }
                actual = a->connections;
            }
            std::vector<std::string> expected;
            for (const usd::Value& i : v->items) {
                expected.push_back(i.text);
            }
            if (actual != expected) {
                std::string got;
                for (const auto& t : actual) {
                    got += " <" + t + ">";
                }
                fail(n, w[1] + " targets:" + got);
            }
        } else if ((op == "api" || op == "noapi") && w.size() == 3) {
            if (const usd::Prim* p = prim(n, w[1]); p && p->hasApiSchema(w[2]) != (op == "api")) {
                fail(n, w[1] + (op == "api" ? " lacks " : " has ") + w[2]);
            }
        } else if (op == "apis" && w.size() >= 2) {
            if (const usd::Prim* p = prim(n, w[1])) {
                const std::vector<std::string> expected(w.begin() + 2, w.end());
                if (p->apiSchemas != expected) {
                    std::string got;
                    for (const auto& s : p->apiSchemas) {
                        got += " " + s;
                    }
                    fail(n, w[1] + " apiSchemas:" + got);
                }
            }
        } else if (op == "variant" && (w.size() == 3 || w.size() == 4)) {
            if (const usd::Prim* p = prim(n, w[1])) {
                const auto it = p->variantSelections.find(w[2]);
                const std::string actual = it == p->variantSelections.end() ? "" : it->second;
                if (actual != (w.size() == 4 ? w[3] : "")) {
                    fail(n, w[1] + " variant " + w[2] + " = '" + actual + "'");
                }
            }
        } else if (op == "children" && w.size() >= 2) {
            const std::vector<std::string> expected(w.begin() + 2, w.end());
            const std::vector<std::string>* actual = nullptr;
            if (w[1] == "/") {
                actual = &m_stage.rootPrims;
            } else if (const usd::Prim* p = prim(n, w[1])) {
                actual = &p->children;
            }
            if (actual && *actual != expected) {
                std::string got;
                for (const auto& s : *actual) {
                    got += " " + s;
                }
                fail(n, w[1] + " children:" + got);
            }
        } else if (op == "diag" && w.size() >= 3) {
            const bool found = std::any_of(m_stage.diagnostics.begin(), m_stage.diagnostics.end(), [&](const usd::Diagnostic& d) {
                const bool sev = (d.severity == usd::Diagnostic::Severity::Error) == (w[1] == "error");
                return sev && d.code == w[2] && (w.size() < 4 || d.primPath == w[3]);
            }) || std::any_of(m_mod.diagnostics.begin(), m_mod.diagnostics.end(), [&](const usd::Diagnostic& d) {
                return w[1] == "warning" && d.code == w[2] && (w.size() < 4 || d.primPath == w[3]);
            });
            if (!found) {
                fail(n, "no " + w[1] + " diagnostic " + w[2] + (w.size() > 3 ? " at " + w[3] : ""));
            }
        } else if (op == "nodiag" && w.size() == 2) {
            for (const usd::Diagnostic& d : m_stage.diagnostics) {
                if (d.code == w[1] || w[1] == "any") {
                    fail(n, "unexpected diagnostic " + d.code + " at " + d.primPath + ": " + d.message);
                }
            }
        } else if (op == "class" && w.size() >= 3) {
            const usd::Classification c = usd::classifyPrimPath(w[1]);
            if (usd::primClassName(c.cls) != w[2]) {
                fail(n, w[1] + " classifies as " + usd::primClassName(c.cls));
            } else if (w.size() >= 4 && fuse::relight::hash::hashToString(c.hash) != w[3]) {
                fail(n, w[1] + " hash " + fuse::relight::hash::hashToString(c.hash));
            }
        } else if (op == "remix" && w.size() == 3) {
            const std::size_t actual = w[1] == "meshes" ? m_mod.meshes.size() : w[1] == "materials" ? m_mod.materials.size() : m_mod.lights.size();
            if (std::to_string(actual) != w[2]) {
                fail(n, "remix " + w[1] + " = " + std::to_string(actual));
            }
        } else if (op == "replacement" && w.size() >= 3) {
            // replacement <mesh|material|light> <hash> [key=value ...]
            one_replacement(n, w);
        } else if (op == "category" && w.size() == 4) {
            if (const usd::Prim* p = prim(n, w[1])) {
                const usd::CategoryOverrides c = usd::readCategories(*p);
                bool found = false;
                for (std::uint32_t i = 0; i < fuse::relight::scene::kInstanceCategoryCount; ++i) {
                    const auto cat = static_cast<fuse::relight::scene::InstanceCategories>(i);
                    if (std::string(usd::remixCategoryAttributeName(cat)) == "remix_category:" + w[2]) {
                        found = true;
                        const std::string actual = !c.exists.test(cat) ? "unset" : (c.flags.test(cat) ? "true" : "false");
                        if (actual != w[3]) {
                            fail(n, w[1] + " category " + w[2] + " is " + actual);
                        }
                    }
                }
                if (!found) {
                    fail(n, "unknown category " + w[2]);
                }
            }
        } else if (op == "particle" && w.size() >= 3) {
            const usd::Prim* p = prim(n, w[1]);
            if (!p) {
                return;
            }
            const auto ps = usd::readParticleSystem(*p);
            if (w[2] == "none") {
                if (ps) {
                    fail(n, w[1] + " has a particle system");
                }
                return;
            }
            if (!ps) {
                fail(n, w[1] + " has no particle system");
                return;
            }
            if (w[2] == "count") {
                if (w.size() != 4 || std::to_string(ps->primvars.size()) != w[3]) {
                    fail(n, w[1] + " particle primvars: " + std::to_string(ps->primvars.size()));
                }
                return;
            }
            const auto v = value(n, rhs(line));
            const usd::Value* a = ps->get(w[2]);
            if (!a) {
                fail(n, w[1] + " has no particle primvar " + w[2]);
            } else if (v && !nearlyEqual(*a, *v)) {
                fail(n, w[1] + " particle " + w[2] + " = " + usd::formatValue(*a));
            }
        } else if (op == "defaultPrim" && w.size() == 2) {
            if (m_stage.defaultPrim != w[1]) {
                fail(n, "defaultPrim is " + m_stage.defaultPrim);
            }
        } else if (op == "layermeta" && w.size() >= 3) {
            const auto v = value(n, rhs(line));
            const auto it = std::find_if(m_stage.layerMetadata.begin(), m_stage.layerMetadata.end(),
                                         [&](const auto& e) { return e.first == w[1]; });
            if (it == m_stage.layerMetadata.end()) {
                fail(n, "no layer metadata " + w[1]);
            } else if (v && !nearlyEqual(it->second, *v)) {
                fail(n, "layer metadata " + w[1] + " = " + usd::formatValue(it->second));
            }
        } else if (op == "customLayerData" && w.size() >= 3) {
            const auto v = value(n, rhs(line));
            const usd::Value* d = m_stage.customLayerData.get(w[1]);
            if (!d) {
                fail(n, "no customLayerData " + w[1]);
            } else if (v && !nearlyEqual(*d, *v)) {
                fail(n, "customLayerData " + w[1] + " = " + usd::formatValue(*d));
            }
        } else if (op == "layers" && w.size() == 2) {
            if (std::to_string(m_stage.layers.size()) != w[1]) {
                fail(n, "opened " + std::to_string(m_stage.layers.size()) + " layers");
            }
        } else if (op == "count" && w.size() == 3) {
            const auto sev = w[1] == "errors" ? usd::Diagnostic::Severity::Error : usd::Diagnostic::Severity::Warning;
            if (std::to_string(m_stage.count(sev)) != w[2]) {
                fail(n, w[1] + " = " + std::to_string(m_stage.count(sev)));
            }
        } else {
            fail(n, "unknown expectation: " + line);
        }
    }

    void one_replacement(int n, const std::vector<std::string>& w) {
        namespace hash = fuse::relight::hash;
        auto kv = [&](const std::string& key) -> std::optional<std::string> {
            for (std::size_t i = 3; i < w.size(); ++i) {
                if (w[i].compare(0, key.size() + 1, key + "=") == 0) {
                    return w[i].substr(key.size() + 1);
                }
            }
            return std::nullopt;
        };
        auto expect = [&](const std::string& key, const std::string& actual) {
            if (auto v = kv(key); v && *v != actual) {
                fail(n, w[1] + " " + w[2] + " " + key + " is '" + actual + "'");
            }
        };
        auto join = [](const std::vector<std::string>& v) {
            std::string s;
            for (const auto& i : v) {
                s += (s.empty() ? "" : ",") + i;
            }
            return s;
        };
        if (w[1] == "mesh") {
            const auto it = std::find_if(m_mod.meshes.begin(), m_mod.meshes.end(), [&](const auto& m) { return hash::hashToString(m.hash) == w[2]; });
            if (it == m_mod.meshes.end()) {
                fail(n, "no mesh replacement " + w[2]);
                return;
            }
            expect("path", it->path);
            expect("meshes", join(it->meshPrims));
            expect("lights", join(it->lightPrims));
            expect("bindings", join(it->materialBindings));
            expect("categories", it->categories.flags.toString());
            expect("particles", it->particles ? "yes" : "no");
            expect("preserve", it->preserveOriginalDrawCall ? (*it->preserveOriginalDrawCall ? "true" : "false") : "unset");
        } else if (w[1] == "material") {
            const auto it = std::find_if(m_mod.materials.begin(), m_mod.materials.end(), [&](const auto& m) {
                return hash::hashToString(m.hash) == w[2] || (w[2] == "origin" && !m.hashFromName);
            });
            if (it == m_mod.materials.end()) {
                fail(n, "no material replacement " + w[2]);
                return;
            }
            expect("path", it->path);
            expect("shader", it->shaderPath);
            expect("mdl", it->mdlSourceAsset);
            expect("sub", it->mdlSubIdentifier);
            expect("key", it->hashFromName ? "name" : "origin");
            expect("particles", it->particles ? "yes" : "no");
        } else if (w[1] == "light") {
            const auto it = std::find_if(m_mod.lights.begin(), m_mod.lights.end(), [&](const auto& l) { return hash::hashToString(l.hash) == w[2]; });
            if (it == m_mod.lights.end()) {
                fail(n, "no light replacement " + w[2]);
                return;
            }
            expect("path", it->path);
            expect("type", it->type);
            expect("legacy", it->legacyName ? "true" : "false");
        } else {
            fail(n, "unknown replacement kind " + w[1]);
        }
    }

    const usd::ComposedStage& m_stage;
    const usd::RemixMod& m_mod;
    bool m_usdc = false;
};

int check(const std::string& root, const fs::path& expectFile, const std::string& sameAs) {
    const usd::ComposedStage stage = usd::readStage(root);
    if (!stage.ok) {
        std::cerr << "check: cannot open " << root << "\n";
        for (const auto& d : stage.diagnostics) {
            std::cerr << "  " << d.code << ": " << d.message << "\n";
        }
        return 1;
    }
    const usd::RemixMod mod = usd::collectRemixMod(stage);
    const auto expect = readFile(expectFile);
    if (!expect) {
        std::cerr << "check: cannot read " << generic(expectFile) << "\n";
        return 2;
    }
    Checker c(stage, mod, fs::path(root).extension() == ".usdc");
    c.run(*expect);
    int failures = c.failures;
    if (!sameAs.empty()) {
        const usd::ComposedStage other = usd::readStage(sameAs);
        const std::string a = comparableDump(stage, c.sameAsIgnoredDiagnostics), b = comparableDump(other, c.sameAsIgnoredDiagnostics);
        if (a != b) {
            ++failures;
            std::istringstream ia(a), ib(b);
            std::string la, lb;
            int line = 0;
            while (true) {
                const bool ha = static_cast<bool>(std::getline(ia, la)), hb = static_cast<bool>(std::getline(ib, lb));
                ++line;
                if (!ha && !hb) {
                    break;
                }
                if (la != lb || ha != hb) {
                    std::cerr << "same-as: dump line " << line << " differs\n  this:  " << (ha ? la : "<eof>") << "\n  other: "
                              << (hb ? lb : "<eof>") << "\n";
                    break;
                }
            }
        }
    }
    if (failures != 0) {
        std::cerr << "check: " << failures << " of " << c.checks << " expectations failed for " << root << "\n";
        for (const auto& d : stage.diagnostics) {
            std::cerr << "  diagnostic " << (d.severity == usd::Diagnostic::Severity::Error ? "error " : "warning ") << d.code
                      << " " << d.primPath << " (" << d.layer << "): " << d.message << "\n";
        }
        return 1;
    }
    std::cout << "check: " << c.checks << " expectations passed for " << root << (sameAs.empty() ? "" : " (dump == " + sameAs + ")")
              << "\n";
    return 0;
}

void printSpec(const usd::PrimSpecData& s, int indent) {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    std::cout << pad << usd::specifierName(s.specifier) << " " << s.typeName << " \"" << s.name << "\"";
    for (const auto& op : s.references) {
        for (const auto& t : op.items) {
            std::cout << " ref(" << usd::listOpKindName(op.kind) << " @" << t.asset << "@<" << t.primPath << ">)";
        }
    }
    for (const auto& op : s.payloads) {
        for (const auto& t : op.items) {
            std::cout << " payload(" << usd::listOpKindName(op.kind) << " @" << t.asset << "@<" << t.primPath << ">)";
        }
    }
    for (const auto& [set, sel] : s.variantSelections) {
        std::cout << " sel(" << set << "=" << sel << ")";
    }
    std::cout << "\n";
    for (const auto& p : s.properties) {
        std::cout << pad << "  ." << p.name << (p.relationship ? " rel" : " " + p.typeName)
                  << (p.hasDefault ? " = " + usd::formatValue(p.defaultValue) : "") << (p.hasTimeSamples ? " (samples)" : "") << "\n";
    }
    for (const auto& [set, variants] : s.variantSets) {
        for (const auto& [name, v] : variants) {
            std::cout << pad << "  variant " << set << "=" << name << ":\n";
            printSpec(v, indent + 2);
        }
    }
    for (const auto& c : s.children) {
        printSpec(c, indent + 1);
    }
}

int usage() {
    std::cerr << "usage: fuse_relight_usd_tool dump <root> [--no-diagnostics] | remix <root> | to-usdc <in> <out> |\n"
                 "       usdc-fixture <src dir> <dst dir> | check <root> <expect> [--same-as <other root>]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> a(argv + 1, argv + argc);
    if (a.empty()) {
        return usage();
    }
    if (a[0] == "dump" && a.size() >= 2) {
        const usd::ComposedStage s = usd::readStage(a[1]);
        std::cout << usd::canonicalDump(s, !(a.size() > 2 && a[2] == "--no-diagnostics"));
        return s.ok ? 0 : 1;
    }
    if (a[0] == "layer" && a.size() == 2) {
        const auto bytes = readFile(a[1]);
        std::string err;
        const auto layer = bytes ? usd::loadLayer(a[1], *bytes, &err) : std::nullopt;
        if (!layer) {
            std::cerr << "layer: " << err << "\n";
            return 1;
        }
        std::cout << "format " << layer->format << " defaultPrim " << layer->defaultPrim << " subLayers " << layer->subLayers.size()
                  << (layer->subLayerOffsets ? " (offsets)" : "") << "\n";
        for (const auto& c : layer->pseudoRoot.children) {
            printSpec(c, 0);
        }
        return 0;
    }
    if (a[0] == "remix" && a.size() == 2) {
        const usd::ComposedStage s = usd::readStage(a[1]);
        printRemix(usd::collectRemixMod(s));
        return s.ok ? 0 : 1;
    }
    if (a[0] == "to-usdc" && a.size() == 3) {
        std::string err;
        if (!toUsdc(a[1], a[2], err)) {
            std::cerr << "to-usdc: " << err << "\n";
            return 2;
        }
        return 0;
    }
    if (a[0] == "usdc-fixture" && a.size() == 3) {
        return usdcFixture(a[1], a[2]);
    }
    if (a[0] == "check" && (a.size() == 3 || (a.size() == 5 && a[3] == "--same-as"))) {
        return check(a[1], a[2], a.size() == 5 ? a[4] : "");
    }
    return usage();
}
