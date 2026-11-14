// fuse_remaster_tscript_dump (plan §1.2 Track B, W1.6): prints deterministic JSON for TorqueScript sources.
//   fuse_remaster_tscript_dump <file>          {"diagnostics", "ast", "analysis"} for one file
//   fuse_remaster_tscript_dump <dir>           {"diagnostics", "call_graph", "datablock_tree"} for every
//                                              *.cs / *.tscript below <dir> (paths relative to <dir>)
// Exit code 1 if any diagnostic was reported, 2 on usage errors.
#include "fuse/remaster/tscript/json.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;
using namespace fuse::remaster::tscript;

namespace {
// Indents a nested JSON document (as produced by json.hpp) by two spaces, without the trailing newline.
std::string nest(std::string doc) {
    if (!doc.empty() && doc.back() == '\n') doc.pop_back();
    std::string out;
    for (const char c : doc) {
        out += c;
        if (c == '\n') out += "  ";
    }
    return out;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file.tscript|file.cs|dir>\n", argv[0]);
        return 2;
    }
    const fs::path in = argv[1];
    std::error_code ec;
    std::vector<Diagnostic> diags;
    std::string out = "{\n";
    if (fs::is_directory(in, ec)) {
        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(in, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            const auto ext = to_lower(it->path().extension().string());
            if (it->is_regular_file(ec) && (ext == ".cs" || ext == ".tscript")) files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        std::vector<FileAnalysis> analyses;
        for (const auto& f : files) {
            auto r = parse_file(f, f.lexically_relative(in).generic_string());
            diags.insert(diags.end(), r.diags.begin(), r.diags.end());
            analyses.push_back(analyze(r));
        }
        out += "  \"diagnostics\": " + nest(diagnostics_to_json(diags)) + ",\n";
        out += "  \"call_graph\": " + nest(call_graph_to_json(build_call_graph(analyses))) + ",\n";
        out += "  \"datablock_tree\": " + nest(datablock_tree_to_json(build_datablock_tree(analyses))) + "\n";
    } else {
        const auto r = parse_file(in, in.filename().generic_string());
        diags = r.diags;
        out += "  \"diagnostics\": " + nest(diagnostics_to_json(r.diags)) + ",\n";
        out += "  \"ast\": " + nest(ast_to_json(*r.root)) + ",\n";
        out += "  \"analysis\": " + nest(analysis_to_json(analyze(r))) + "\n";
    }
    out += "}\n";
#ifdef _WIN32
    (void)_setmode(_fileno(stdout), _O_BINARY); // byte-identical output on every platform
#endif
    std::fwrite(out.data(), 1, out.size(), stdout);
    for (const auto& d : diags) std::fprintf(stderr, "%s:%u:%u: error: %s\n", d.file.c_str(), d.line, d.col, d.message.c_str());
    return diags.empty() ? 0 : 1;
}
