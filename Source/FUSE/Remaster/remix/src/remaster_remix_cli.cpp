// FUSE Remaster W2.3 / W2.4: the `ingest-remix` and `export-remix` subcommands of fuse_remaster (linked into the CLI
// when FUSE Relight is built; see Source/FUSE/Remaster/remix/CMakeLists.txt).
//
//   fuse_remaster ingest-remix <capture dir | stage> --project <dir> [--rule <asset hash rule>] [--json]
//   fuse_remaster export-remix --project <dir> --out <mod dir> [--mod-name <name>] [--local] [--json]
//       export, then checkExport (manifest schema, DDS, composed stage, re-ingest, no-originals lint); exit 1 on
//       any finding.
#include <fuse/remaster/remix/remix.hpp>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

struct RemixArgs {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
};

RemixArgs parse(int argc, char** argv) {
    RemixArgs a;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        if (s.starts_with("--")) {
            const std::string key = s.substr(2);
            if (key != "json" && key != "local" && i + 1 < argc) {
                a.options[key] = argv[++i];
            } else {
                a.options[key] = "";
            }
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

namespace rx = fuse::remaster::remix;

} // namespace

int fuseRemasterIngestRemix(int argc, char** argv) {
    const RemixArgs a = parse(argc, argv);
    if (a.positional.size() != 1 || !a.options.contains("project")) {
        std::fprintf(stderr, "usage: fuse_remaster ingest-remix <capture dir> --project <dir> [--rule <rule>] [--json]\n");
        return 2;
    }
    rx::IngestOptions o;
    o.captureDir = a.positional[0];
    o.projectDir = a.options.at("project");
    o.rule = a.options.contains("rule") ? a.options.at("rule") : "";
    const rx::IngestReport r = rx::ingestRemix(o);
    if (a.options.contains("json")) {
        std::fputs(fuse::remaster::json::writePretty(rx::toJson(r)).c_str(), stdout);
    } else {
        for (const auto& asset : r.assets) {
            std::printf("%s %s", asset.oaid.c_str(), asset.kind.c_str());
            for (const auto& [algo, value] : asset.keys) {
                std::printf(" %s:%s", algo.c_str(), value.c_str());
            }
            std::printf("\n");
        }
        for (const auto& w : r.warnings) {
            std::fprintf(stderr, "warning: %s\n", w.c_str());
        }
        for (const auto& e : r.errors) {
            std::fprintf(stderr, "error: %s\n", e.c_str());
        }
        std::printf("ingest-remix: %zu original assets (%zu new), %zu records, %zu blobs%s\n", r.assets.size(), r.newAssets,
                    r.records, r.blobs, r.ok ? "" : " - FAILED");
    }
    return r.ok ? 0 : 1;
}

int fuseRemasterExportRemix(int argc, char** argv) {
    const RemixArgs a = parse(argc, argv);
    if (!a.options.contains("project") || !a.options.contains("out")) {
        std::fprintf(stderr, "usage: fuse_remaster export-remix --project <dir> --out <mod dir> [--mod-name <n>] [--local] [--json]\n");
        return 2;
    }
    rx::ExportOptions o;
    o.projectDir = a.options.at("project");
    o.outDir = a.options.at("out");
    if (a.options.contains("mod-name")) {
        o.modName = a.options.at("mod-name");
    }
    o.local = a.options.contains("local");
    const rx::ExportReport r = rx::exportRemix(o);
    rx::CheckReport c;
    if (r.ok) {
        c = rx::checkExport(o.outDir, o.projectDir);
    }
    std::vector<std::string> errors = r.errors;
    for (const auto& e : c.errors) {
        errors.push_back("check: " + e);
    }
    if (a.options.contains("json")) {
        fuse::remaster::json::Value out = fuse::remaster::json::Value::object();
        out["schema"] = "fuse.remaster.remix-export-report/1";
        out["ok"] = errors.empty();
        fuse::remaster::json::Value reps = fuse::remaster::json::Value::array(), files = fuse::remaster::json::Value::array(),
                                    errs = fuse::remaster::json::Value::array(), found = fuse::remaster::json::Value::array();
        for (const auto& x : r.replacements) {
            fuse::remaster::json::Value v = fuse::remaster::json::Value::object();
            v["oaid"] = x.oaid;
            v["kind"] = x.kind;
            v["prim"] = x.prim;
            reps.push(std::move(v));
        }
        for (const auto& f : r.files) {
            files.push(f);
        }
        for (const auto& e : errors) {
            errs.push(e);
        }
        for (const auto& f : c.foundOaids) {
            found.push(f);
        }
        out["replacements"] = std::move(reps);
        out["files"] = std::move(files);
        out["reingest_oaids"] = std::move(found);
        out["errors"] = std::move(errs);
        std::fputs(fuse::remaster::json::writePretty(out).c_str(), stdout);
    } else {
        for (const auto& x : r.replacements) {
            std::printf("%s %s -> %s/%s\n", x.prim.c_str(), x.oaid.c_str(), x.pocoKind.c_str(), x.pocoId.c_str());
        }
        for (const auto& e : errors) {
            std::fprintf(stderr, "error: %s\n", e.c_str());
        }
        std::printf("export-remix: %zu replacements, %zu files, re-ingest found %zu%s\n", r.replacements.size(), r.files.size(),
                    c.foundOaids.size(), errors.empty() ? "" : " - FAILED");
    }
    return errors.empty() ? 0 : 1;
}
