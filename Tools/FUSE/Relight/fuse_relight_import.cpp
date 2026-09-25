// FUSE Relight RL-3.2: fuse_relight_import, the mod importer CLI (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.8).
//
//   fuse_relight_import <mod dir | stage file> --game <id> --out <store dir>
//                       [--name <mod name>] [--rule <geometry asset rule>] [--as-mod | --as-capture]
//                       [--no-merge] [--verify] [--quiet]
//   fuse_relight_import verify <store dir>
//
// Imports a Remix mod (a directory with mod.usda / mod.usdc / mod.usd, or a stage file) or a capture (a stage
// whose lightspeed_layer_type is "capture"; RL-1.8 captures included) into the POCO store shim at --out: POCO
// records, content-addressed blobs and the replacement-DB rows (db/remaster_db.json, merged with an existing DB
// of the same game unless --no-merge). --verify re-reads the written store (mods::import::verifyStore).
//
// Output: one summary line, then one line per diagnostic ("warning <code> <where>: <message>"). Exit codes: 0 ok
// (warnings allowed), 1 import errors or a failed verification, 2 usage.
#include <fuse/relight/mods/import/mod_importer.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

namespace imp = fuse::relight::mods::import;

int usage() {
    std::fprintf(stderr,
                 "usage: fuse_relight_import <mod dir | stage file> --game <id> --out <store dir>\n"
                 "                           [--name <mod name>] [--rule <geometry asset rule>] [--as-mod | --as-capture]\n"
                 "                           [--no-merge] [--verify] [--quiet]\n"
                 "       fuse_relight_import verify <store dir>\n");
    return 2;
}

int verify(const std::string& dir, bool quiet) {
    const imp::VerifyResult v = imp::verifyStore(dir);
    if (!quiet || !v.ok()) {
        std::printf("verify %s: %zu records, %zu blobs, %zu keys, %zu replacements, %zu errors\n", dir.c_str(), v.records,
                    v.blobs, v.keys, v.replacements, v.errors.size());
    }
    for (const std::string& e : v.errors) {
        std::printf("error %s\n", e.c_str());
    }
    return v.ok() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "verify") == 0) {
        return argc == 3 ? verify(argv[2], false) : usage();
    }
    imp::ImportOptions opt;
    std::string out;
    bool merge = true, doVerify = false, quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) {
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (a == "--game") {
            if (!value(opt.gameId)) {
                return usage();
            }
        } else if (a == "--out") {
            if (!value(out)) {
                return usage();
            }
        } else if (a == "--name") {
            if (!value(opt.modName)) {
                return usage();
            }
        } else if (a == "--rule") {
            if (!value(opt.assetRule)) {
                return usage();
            }
        } else if (a == "--as-mod") {
            opt.kind = imp::ImportKind::Mod;
        } else if (a == "--as-capture") {
            opt.kind = imp::ImportKind::Capture;
        } else if (a == "--no-merge") {
            merge = false;
        } else if (a == "--verify") {
            doVerify = true;
        } else if (a == "--quiet") {
            quiet = true;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "fuse_relight_import: unknown option %s\n", a.c_str());
            return usage();
        } else if (opt.root.empty()) {
            opt.root = a;
        } else {
            return usage();
        }
    }
    if (opt.root.empty() || opt.gameId.empty() || out.empty()) {
        return usage();
    }
    const imp::ImportResult r = imp::importMod(opt);
    const auto& c = r.counts;
    std::printf("%s %s: %zu mesh replacements, %zu meshes, %zu materials, %zu textures, %zu lights, %zu particle systems, "
                "%zu records, %zu blobs, %zu keys, %zu replacement rows, licence %s, %zu diagnostics\n",
                r.capture ? "capture" : "mod", r.idPrefix.c_str(), c.meshReplacements, c.meshes, c.materials, c.textures,
                c.lights, c.particles, c.records, c.blobs, c.keys, c.replacements, r.licenceId.c_str(), r.diagnostics.size());
    if (!quiet) {
        for (const imp::ImportDiagnostic& d : r.diagnostics) {
            std::printf("%s %s %s: %s\n", d.severity.c_str(), d.code.c_str(), d.where.c_str(), d.message.c_str());
        }
    }
    if (!r.ok) {
        return 1;
    }
    std::string err;
    if (!imp::writeStore(out, r, merge, &err)) {
        std::fprintf(stderr, "fuse_relight_import: %s\n", err.c_str());
        return 1;
    }
    int rc = r.errors() == 0 ? 0 : 1;
    if (doVerify && verify(out, quiet) != 0) {
        rc = 1;
    }
    return rc;
}
