// FUSE Relight RL-6.3: fuse_relight_setup, the game-setup assistant and profile tool.
//
//   fuse_relight_setup analyze <relight_capture.jsonl> --exe <name> [--exe-file <path>] [--name <profile name>]
//                              [--threshold <0..1>] [--out <profile.json>]
//       Print the assistant report and write the proposed profile (applied + pending suggestions).
//   fuse_relight_setup accept <profile.json> <selector> [--out <path>]
//       Apply pending suggestions: a hash (0x...), an option key, a category name, or "all".
//   fuse_relight_setup export-conf <profile.json> [--out <rtx.conf>]
//       The profile's rtx.conf layer as Remix rtx.conf text.
//   fuse_relight_setup import-conf <rtx.conf> --exe <name> [--name <profile name>] [--out <profile.json>]
//       A profile from an rtx.conf (texture lists become categories, the rest options).
//   fuse_relight_setup match --exe <path> [--dir <dir>]... [--exe-hash 0x...]
//       Which profile discovery picks for that executable (exit 0 matched, 3 none).
//   fuse_relight_setup hash-exe <path>
//       XXH3-64 of the file (the value for "match.xxh3").
//   fuse_relight_setup check <profile.json>
//       Parse and verify the file is in canonical form (exit 1 otherwise).
#include <fuse/relight/setup/profile.hpp>
#include <fuse/relight/setup/profile_discovery.hpp>
#include <fuse/relight/setup/setup_assistant.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::relight::setup;

int usage() {
    std::fprintf(stderr,
                 "usage: fuse_relight_setup analyze <capture.jsonl> --exe <name> [--exe-file <path>] [--name <n>] "
                 "[--threshold <t>] [--out <profile.json>]\n"
                 "       fuse_relight_setup accept <profile.json> <selector> [--out <path>]\n"
                 "       fuse_relight_setup export-conf <profile.json> [--out <rtx.conf>]\n"
                 "       fuse_relight_setup import-conf <rtx.conf> --exe <name> [--name <n>] [--out <profile.json>]\n"
                 "       fuse_relight_setup match --exe <path> [--dir <dir>]... [--exe-hash 0x...]\n"
                 "       fuse_relight_setup hash-exe <path>\n"
                 "       fuse_relight_setup check <profile.json>\n");
    return 2;
}

struct Args {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> flags;
    std::string get(const std::string& name, const std::string& fallback = {}) const {
        for (auto it = flags.rbegin(); it != flags.rend(); ++it) {
            if (it->first == name) {
                return it->second;
            }
        }
        return fallback;
    }
    std::vector<std::string> all(const std::string& name) const {
        std::vector<std::string> out;
        for (const auto& [k, v] : flags) {
            if (k == name) {
                out.push_back(v);
            }
        }
        return out;
    }
};

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "fuse_relight_setup: %s needs a value\n", a.c_str());
                return false;
            }
            args.flags.emplace_back(a, argv[++i]);
        } else {
            args.positional.push_back(a);
        }
    }
    return true;
}

bool writeText(const std::string& path, const std::string& text) {
    if (path.empty() || path == "-") {
        std::fwrite(text.data(), 1, text.size(), stdout);
        return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        std::fprintf(stderr, "fuse_relight_setup: cannot write %s\n", path.c_str());
        return false;
    }
    return true;
}

bool readText(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "fuse_relight_setup: cannot read %s\n", path.c_str());
        return false;
    }
    std::ostringstream s;
    s << in.rdbuf();
    text = s.str();
    return true;
}

std::optional<GameProfile> load(const std::string& path) {
    std::string error;
    std::optional<GameProfile> p = loadProfileFile(path, &error);
    if (!p) {
        std::fprintf(stderr, "fuse_relight_setup: %s\n", error.c_str());
    }
    return p;
}

int cmdAnalyze(const Args& args) {
    if (args.positional.size() != 1 || args.get("--exe").empty()) {
        return usage();
    }
    std::string error;
    const std::optional<SetupReport> report = analyzeCaptureFile(args.positional[0], &error);
    if (!report) {
        std::fprintf(stderr, "fuse_relight_setup: %s\n", error.c_str());
        return 1;
    }
    std::fputs(formatReport(*report).c_str(), stderr);
    std::optional<std::uint64_t> exeHash;
    if (const std::string exeFile = args.get("--exe-file"); !exeFile.empty()) {
        exeHash = hashExecutableFile(exeFile);
        if (!exeHash) {
            std::fprintf(stderr, "fuse_relight_setup: cannot hash %s\n", exeFile.c_str());
            return 1;
        }
    }
    AssistantOptions options;
    if (const std::string t = args.get("--threshold"); !t.empty()) {
        options.applyThreshold = std::strtod(t.c_str(), nullptr);
    }
    const std::string exe = args.get("--exe");
    std::string name = args.get("--name");
    if (name.empty()) {
        name = exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0 ? exe.substr(0, exe.size() - 4) : exe;
    }
    return writeText(args.get("--out"), writeProfile(proposeProfile(*report, name, exe, exeHash, options))) ? 0 : 1;
}

int cmdAccept(const Args& args) {
    if (args.positional.size() != 2) {
        return usage();
    }
    std::optional<GameProfile> p = load(args.positional[0]);
    if (!p) {
        return 1;
    }
    const std::size_t n = p->acceptSuggestions(args.positional[1]);
    std::fprintf(stderr, "fuse_relight_setup: applied %zu suggestion(s)\n", n);
    return writeText(args.get("--out", args.positional[0]), writeProfile(*p)) ? 0 : 1;
}

int cmdExportConf(const Args& args) {
    if (args.positional.size() != 1) {
        return usage();
    }
    const std::optional<GameProfile> p = load(args.positional[0]);
    return p && writeText(args.get("--out"), exportRtxConf(*p)) ? 0 : 1;
}

int cmdImportConf(const Args& args) {
    if (args.positional.size() != 1 || args.get("--exe").empty()) {
        return usage();
    }
    std::string text;
    if (!readText(args.positional[0], text)) {
        return 1;
    }
    fuse::relight::options::ConfigParseOptions parse;
    parse.exeName = args.get("--exe");
    std::vector<fuse::relight::options::ConfigDiagnostic> diagnostics;
    GameProfile p = importRtxConf(text, parse, &diagnostics);
    for (const auto& d : diagnostics) {
        std::fprintf(stderr, "%s:%u: %s\n", args.positional[0].c_str(), d.line, d.message.c_str());
    }
    p.name = args.get("--name", parse.exeName);
    p.match.exeNames.push_back(parse.exeName);
    return writeText(args.get("--out"), writeProfile(p)) ? 0 : 1;
}

int cmdMatch(const Args& args) {
    if (args.get("--exe").empty()) {
        return usage();
    }
    DiscoveryRequest request;
    request.exe.path = args.get("--exe");
    if (const std::string h = args.get("--exe-hash"); !h.empty()) {
        std::uint64_t value = 0;
        if (!parseHash(h, value)) {
            return usage();
        }
        request.exe.hash = value;
    }
    request.searchDirs = args.all("--dir");
    if (request.searchDirs.empty()) {
        request.searchDirs = defaultProfileSearchDirs(request.exe.path);
    }
    const DiscoveryResult result = discoverProfile(request);
    for (const std::string& line : result.log) {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    if (!result.profile) {
        std::printf("none\n");
        return 3;
    }
    std::printf("%s %s %s\n", result.score == 2 ? "hash" : "name", result.profile->sourcePath.c_str(),
                result.profile->name.c_str());
    return 0;
}

int cmdHashExe(const Args& args) {
    if (args.positional.size() != 1) {
        return usage();
    }
    const std::optional<std::uint64_t> h = hashExecutableFile(args.positional[0]);
    if (!h) {
        std::fprintf(stderr, "fuse_relight_setup: cannot read %s\n", args.positional[0].c_str());
        return 1;
    }
    std::printf("%s\n", formatHash(*h).c_str());
    return 0;
}

int cmdCheck(const Args& args) {
    if (args.positional.size() != 1) {
        return usage();
    }
    std::string text;
    if (!readText(args.positional[0], text)) {
        return 1;
    }
    std::string error;
    const std::optional<GameProfile> p = parseProfile(text, &error);
    if (!p) {
        std::fprintf(stderr, "%s: %s\n", args.positional[0].c_str(), error.c_str());
        return 1;
    }
    if (writeProfile(*p) != text) {
        std::fprintf(stderr, "%s: not in canonical form (rewrite it with fuse_relight_setup accept <file> none)\n",
                     args.positional[0].c_str());
        return 1;
    }
    std::printf("%s: ok\n", args.positional[0].c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    Args args;
    if (!parseArgs(argc, argv, args)) {
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "analyze") return cmdAnalyze(args);
    if (cmd == "accept") return cmdAccept(args);
    if (cmd == "export-conf") return cmdExportConf(args);
    if (cmd == "import-conf") return cmdImportConf(args);
    if (cmd == "match") return cmdMatch(args);
    if (cmd == "hash-exe") return cmdHashExe(args);
    if (cmd == "check") return cmdCheck(args);
    return usage();
}
