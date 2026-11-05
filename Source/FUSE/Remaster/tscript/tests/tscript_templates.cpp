// Gate remaster_tscript_templates (plan §1.2 Track B, W1.6 exit criterion): parses every TorqueScript file
// (*.cs, *.tscript) under the given Templates directory, fails on any diagnostic or if fewer than 500 files
// were found, and prints the file / function / datablock / call-edge totals. Exit 77 (ctest SKIP) when the
// directory does not exist.
#include "fuse/remaster/tscript/analysis.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fuse::remaster::tscript;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <Templates dir> [min files]\n", argv[0]);
        return 2;
    }
    const fs::path root = argv[1];
    const std::size_t min_files = argc > 2 ? static_cast<std::size_t>(std::stoul(argv[2])) : 500;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::printf("SKIP: %s is not a directory\n", root.generic_string().c_str());
        return 77;
    }

    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const auto ext = to_lower(it->path().extension().string());
        if (ext == ".cs" || ext == ".tscript") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    std::vector<FileAnalysis> analyses;
    analyses.reserve(files.size());
    std::size_t diag_count = 0, failed_files = 0, lines = 0;
    for (const auto& f : files) {
        const auto rel = f.lexically_relative(root).generic_string();
        const auto r = parse_file(f, rel);
        lines += static_cast<std::size_t>(std::count(r.source.begin(), r.source.end(), '\n'));
        if (!r.ok()) {
            ++failed_files;
            for (const auto& d : r.diags) {
                if (diag_count++ < 100) {
                    std::printf("%s:%u:%u: error: %s\n", d.file.c_str(), d.line, d.col, d.message.c_str());
                }
            }
        }
        analyses.push_back(analyze(r));
    }

    const auto graph = build_call_graph(analyses);
    const auto tree = build_datablock_tree(analyses);
    std::size_t functions = 0, datablocks = 0, singletons = 0, named_new = 0;
    for (const auto& [k, defs] : graph.definitions) functions += defs.size();
    for (const auto& n : tree.nodes) {
        if (n.def.kind == "datablock") ++datablocks;
        else if (n.def.kind == "singleton") ++singletons;
        else ++named_new;
    }
    std::size_t method_edges = 0;
    for (const auto& e : graph.edges) method_edges += e.kind == CallKind::Method ? 1 : 0;

    std::printf("files parsed:         %zu (%zu lines)\n", files.size(), lines);
    std::printf("files with errors:    %zu (%zu diagnostics)\n", failed_files, diag_count);
    std::printf("functions defined:    %zu (%zu distinct keys, %zu packages)\n", functions,
                graph.definitions.size(), graph.packages.size());
    std::printf("call edges:           %zu (%zu method calls, %zu unresolved callee keys, %u unresolved parent::)\n",
                graph.edges.size(), method_edges, graph.unresolved.size(), graph.unresolved_parent_calls);
    std::printf("datablocks:           %zu\n", datablocks);
    std::printf("singletons:           %zu\n", singletons);
    std::printf("named file-scope new: %zu\n", named_new);
    std::printf("tree: %zu roots, %zu missing parents, %u duplicate names\n", tree.roots.size(),
                tree.missing_parents.size(), tree.duplicate_names);

    bool ok = true;
    if (files.size() < min_files) {
        std::printf("FAIL: expected >= %zu TorqueScript files, found %zu\n", min_files, files.size());
        ok = false;
    }
    if (diag_count != 0) {
        std::printf("FAIL: %zu diagnostics in %zu files\n", diag_count, failed_files);
        ok = false;
    }
    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
