// fuse_lint — dependency-free source/target lint gates for the FUSE master plan.
//
//   fuse_lint <check> [--root DIR] [--dir DIR] [--manifest FILE] [--repo DIR]
//                     [--plan FILE] [--sources DIR] --scratch DIR
//
// Every check first runs a self-test: it seeds a known-bad and a known-good sample tree under
// --scratch and must flag the bad one and pass the good one, then it scans the real inputs.
// Exit code 0 = self-test ok and no violations; 1 = violations; 2 = self-test / usage failure.
//
// Checks (master plan rows):
//   ownership      No owning raw pointers in public FUSE APIs (one include tree per run).
//   namespace      Appendix A: namespace `fuse::` (public headers + product sources).
//   macros         Appendix A: macros FUSE_* / FUSE_ASSERT / FUSE_HOST_DEVICE.
//   torque-macros  Appendix A: TORQUE_* only inside compat/ (Compat/ and Legacy/ quarantine).
//   torque-names   Appendix A: log channels / memory domains renamed (no Torque names).
//   banned-deps    Appendix A: no Meridian; no ImGui (Source/FUSE text + fuse_* link graph).
//   cxx-standard   Appendix C: CMAKE_CXX_STANDARD 23 on all non-CUDA fuse_* host targets.
//   qt-includes    Appendix C: no #include <Q*> in core/renderer/physics/ecs/compute.
//   editor-qt6     B6 "Qt 6 only for editor chrome — no Dear ImGui": fuse_editor's transitive link
//                  closure has Qt6::Widgets, no ImGui and no non-Qt6 Qt; Editor sources include no
//                  ImGui, Qt only in the Qt host (src/qt/, *_qt.*), and the Qt host includes only
//                  FUSE public headers, Qt, Vulkan and the standard library (B6.1 on FUSE APIs).
//                  Exit 77 when fuse_editor is not configured (Qt6 not found / FUSE_BUILD_EDITOR=OFF).
//   doc-headings   Appendix C: every `### B*.*` has a matching `## N.M` in docs/sources/P*.md.
//   vendored-pins  B1 gate "Third-party dependencies build from vendored source with pinned commits":
//                  --dir <vendored lib> holds a VERSION pin (upstream, version, tag, 40-hex commit,
//                  license, sha256 per file) that matches the vendored files and the version the
//                  header itself declares (or `header_version`, for one component of a larger SDK tag).
//   branding       Appendix A "Icons, installer, docs, CI badge names": --repo workflows' `name:`
//                  fields say FUSE (no Torque/T3D job / step names), README.md is titled `# FUSE`
//                  with one CI badge per workflow (alt = workflow name), docs titles say FUSE.
//   asset-licences FUSE_ASSET_PLAN §2.4 / W0.5: --root Content holds licences.lock.json; every file under it
//                  resolves to a record with a matching sha256 and an allowed (or reviewed) licence, no NC /
//                  ND / editorial / research-only / unknown-provenance entries, no share-alike leakage over
//                  derived_from, generator records carry path@revision + seed, CREDITS.md is current.
//                  Optional --manifest <cook_manifest.json> (cooked outputs need a record) and --cache DIR.
//                  (fuse_lint_asset_licences.cpp)
//   asset-credits  Regenerates --root Content/CREDITS.md from the lock file (not a gate; exit 0 on success).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "fuse_lint_asset_licences.hpp"

namespace fs = std::filesystem;

namespace {

struct Args {
    std::string check;
    fs::path root, dir, manifest, repo, plan, sources, scratch, cache;
};

using Violations = std::vector<std::string>;

// ---- helpers ------------------------------------------------------------------------------------

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string rel(const fs::path& p, const fs::path& base) {
    std::error_code ec;
    fs::path r = fs::relative(p, base, ec);
    return (ec || r.empty() ? p : r).generic_string();
}

bool hasExt(const fs::path& p, std::initializer_list<const char*> exts) {
    const std::string e = lower(p.extension().string());
    for (const char* x : exts) {
        if (e == x) {
            return true;
        }
    }
    return false;
}

bool isHeader(const fs::path& p) { return hasExt(p, {".hpp", ".h", ".hh", ".hxx", ".inl"}); }
bool isCxxSource(const fs::path& p) {
    return isHeader(p) || hasExt(p, {".cpp", ".cc", ".cxx", ".cu", ".cuh", ".mm", ".ipp"});
}

/// Files under `dir` (recursive, sorted) accepted by `pred(path, generic relative path)`.
std::vector<fs::path> listFiles(const fs::path& dir, const std::function<bool(const fs::path&, const std::string&)>& pred) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string r = "/" + rel(it->path(), dir);
        if (pred(it->path(), r)) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Top-level Source/FUSE directories that are Torque quarantine (compat loaders, legacy engines).
bool inQuarantine(const std::string& relFromRoot) {
    return relFromRoot.rfind("/Compat/", 0) == 0 || relFromRoot.rfind("/Legacy/", 0) == 0;
}

/// A source split into lines: `raw` as written, `code` with comments removed, and `bare` with
/// comments removed and string/char literal contents blanked (quotes kept, length preserved).
struct Line {
    std::string raw, code, bare;
};

std::vector<Line> scanLines(const std::string& text) {
    std::vector<Line> lines(1);
    enum class St { Code, LineComment, BlockComment, Str, Chr, Raw } st = St::Code;
    std::string rawDelim;
    auto emit = [&](char c, bool inCode, bool inLiteral) {
        Line& l = lines.back();
        l.raw.push_back(c);
        if (inCode || inLiteral) {
            l.code.push_back(c);
        }
        if (inCode) {
            l.bare.push_back(c);
        } else if (inLiteral) {
            l.bare.push_back(' ');
        }
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char n = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (st == St::LineComment) {
                st = St::Code;
            }
            if (st == St::Str || st == St::Chr) {
                st = St::Code; // unterminated literal: recover at end of line
            }
            lines.emplace_back();
            continue;
        }
        switch (st) {
        case St::Code:
            if (c == '/' && n == '/') {
                st = St::LineComment;
                lines.back().raw += "//";
                ++i;
            } else if (c == '/' && n == '*') {
                st = St::BlockComment;
                lines.back().raw += "/*";
                lines.back().code += ' ';
                lines.back().bare += ' ';
                ++i;
            } else if (c == 'R' && n == '"' && (i == 0 || !(std::isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_'))) {
                const size_t open = text.find('(', i + 2);
                if (open == std::string::npos || open - (i + 2) > 16) {
                    emit(c, true, false);
                    break;
                }
                rawDelim = ")" + text.substr(i + 2, open - (i + 2)) + "\"";
                for (size_t k = i; k <= open; ++k) {
                    emit(text[k], true, false);
                }
                i = open;
                st = St::Raw;
            } else if (c == '"') {
                emit(c, true, false);
                st = St::Str;
            } else if (c == '\'') {
                // C++14 digit separator (1'000) is not a char literal.
                const bool sep = i > 0 && std::isxdigit((unsigned char)text[i - 1]) && std::isxdigit((unsigned char)n);
                emit(c, true, false);
                if (!sep) {
                    st = St::Chr;
                }
            } else {
                emit(c, true, false);
            }
            break;
        case St::LineComment:
            lines.back().raw.push_back(c);
            break;
        case St::BlockComment:
            lines.back().raw.push_back(c);
            if (c == '*' && n == '/') {
                lines.back().raw.push_back('/');
                ++i;
                st = St::Code;
            }
            break;
        case St::Str:
        case St::Chr: {
            const char q = st == St::Str ? '"' : '\'';
            if (c == '\\' && n != '\0' && n != '\n') {
                emit(c, false, true);
                emit(n, false, true);
                ++i;
            } else if (c == q) {
                emit(c, true, false);
                st = St::Code;
            } else {
                emit(c, false, true);
            }
            break;
        }
        case St::Raw:
            if (text.compare(i, rawDelim.size(), rawDelim) == 0) {
                for (char d : rawDelim) {
                    emit(d, true, false);
                }
                i += rawDelim.size() - 1;
                st = St::Code;
            } else {
                emit(c, false, true);
            }
            break;
        }
    }
    return lines;
}

std::string trimLeft(const std::string& s) {
    const size_t p = s.find_first_not_of(" \t");
    return p == std::string::npos ? std::string() : s.substr(p);
}

/// Preprocessor directive lines (including backslash continuations) are flagged true.
std::vector<bool> preprocessorMask(const std::vector<Line>& lines) {
    std::vector<bool> mask(lines.size(), false);
    bool cont = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trimLeft(lines[i].code);
        const bool pp = cont || (!t.empty() && t[0] == '#');
        mask[i] = pp;
        cont = pp && !lines[i].code.empty() && lines[i].code.back() == '\\';
    }
    return mask;
}

/// Inline allowlist marker: `// fuse-lint-allow(<check>): <justification, >= 10 chars>`.
bool inlineAllowed(const std::string& raw, const std::string& check) {
    const std::string tag = "fuse-lint-allow(" + check + "):";
    const size_t p = raw.find(tag);
    if (p == std::string::npos) {
        return false;
    }
    return trimLeft(raw.substr(p + tag.size())).size() >= 10u;
}

std::string where(const fs::path& file, const fs::path& base, size_t lineIdx) {
    return rel(file, base) + ":" + std::to_string(lineIdx + 1);
}

// ---- check: ownership -----------------------------------------------------------------------------

/// Central allowlist for non-owning raw-pointer accessors whose names look like factories.
/// Each entry: path suffix, code substring, justification. Prefer the inline marker for new code.
struct AllowEntry {
    const char* pathSuffix;
    const char* code;
    const char* why;
};
constexpr AllowEntry kOwnershipAllow[] = {
    // (empty today — the tree is clean; add {suffix, code, justification} rows here when a
    // non-owning view legitimately matches the factory pattern.)
    {"/fuse_lint_selftest_allow.hpp", "Widget* openView()", "self-test: non-owning view into a pool"},
};

Violations checkOwnership(const fs::path& dir) {
    const std::regex factory(R"([A-Za-z0-9_>]\s*\*\s*(create|make|clone|spawn|instantiate|open|load|acquire)[A-Za-z0-9_]*\s*\()");
    const std::regex naked(R"((^|[^A-Za-z0-9_:])(new\s+[A-Za-z_:][A-Za-z0-9_:<>]*\s*[\[({;]|delete(\[\])?\s+[A-Za-z_(*]))");
    const std::regex rawRelease(R"(\*\s*release\s*\(\s*\)\s*(const\s*)?(noexcept\s*)?[;{])");
    Violations v;
    const auto files = listFiles(dir, [](const fs::path& p, const std::string& r) {
        return isHeader(p) && r.find("/tests/") == std::string::npos && !r.ends_with("/fuse/alloc/new_ban.hpp");
    });
    for (const fs::path& f : files) {
        const auto lines = scanLines(readFile(f));
        const auto pp = preprocessorMask(lines);
        const std::string gen = f.generic_string();
        for (size_t i = 0; i < lines.size(); ++i) {
            if (pp[i] || trimLeft(lines[i].bare).empty()) {
                continue;
            }
            const std::string& code = lines[i].bare;
            if (!std::regex_search(code, factory) && !std::regex_search(code, naked) && !std::regex_search(code, rawRelease)) {
                continue;
            }
            if (inlineAllowed(lines[i].raw, "ownership")) {
                continue;
            }
            bool listed = false;
            for (const AllowEntry& a : kOwnershipAllow) {
                listed |= gen.ends_with(a.pathSuffix) && code.find(a.code) != std::string::npos;
            }
            if (!listed) {
                v.push_back(where(f, dir, i) + ": owning raw pointer: " + trimLeft(lines[i].raw));
            }
        }
    }
    if (files.empty()) {
        v.push_back(dir.generic_string() + ": no public headers found (wrong --dir?)");
    }
    return v;
}

// ---- check: namespace ------------------------------------------------------------------------------

/// Product (non-quarantine) files under Source/FUSE: public headers under */include and sources under
/// */src. Apps/, tests/ and samples are excluded (executables may use global scope freely).
std::vector<fs::path> productFiles(const fs::path& root, bool headersOnly) {
    return listFiles(root, [&](const fs::path& p, const std::string& r) {
        if (inQuarantine(r) || r.find("/tests/") != std::string::npos || r.rfind("/Apps/", 0) == 0) {
            return false;
        }
        if (r.find("/include/") != std::string::npos) {
            return isHeader(p);
        }
        return !headersOnly && r.find("/src/") != std::string::npos && isCxxSource(p);
    });
}

Violations checkNamespace(const fs::path& root) {
    Violations v;
    const std::regex tok(
        R"(\bnamespace\s+([A-Za-z_][A-Za-z0-9_:]*)?\s*(\[\[[^\]]*\]\]\s*)?\{|\bextern\s+"[^"]*"\s*\{|\b(class|struct|union|enum(?:\s+class|\s+struct)?)\s+(?:alignas\s*\([^)]*\)\s*|\[\[[^\]]*\]\]\s*|FUSE_[A-Z_]+\s+)*([A-Za-z_][A-Za-z0-9_]*)(::)?[^;{}()=]*\{|[{}])");
    const auto files = productFiles(root, false);
    size_t headers = 0;
    for (const fs::path& f : files) {
        const bool header = isHeader(f);
        headers += header ? 1u : 0u;
        const auto lines = scanLines(readFile(f));
        const auto pp = preprocessorMask(lines);
        std::string joined;
        std::vector<size_t> lineStart;
        for (size_t i = 0; i < lines.size(); ++i) {
            lineStart.push_back(joined.size());
            joined += pp[i] ? std::string() : lines[i].bare;
            joined += '\n';
        }
        auto lineOf = [&](size_t off) {
            return size_t(std::upper_bound(lineStart.begin(), lineStart.end(), off) - lineStart.begin()) - 1u;
        };
        std::vector<char> stack; // N namespace, E extern "C", T type, B other
        bool anyFuse = false;
        for (auto it = std::sregex_iterator(joined.begin(), joined.end(), tok); it != std::sregex_iterator(); ++it) {
            const std::smatch& m = *it;
            const std::string s = m.str(0);
            const size_t li = lineOf(size_t(m.position(0)));
            const bool inNs = std::find(stack.begin(), stack.end(), 'N') != stack.end();
            if (s == "}") {
                if (!stack.empty()) {
                    stack.pop_back();
                }
            } else if (s == "{") {
                stack.push_back('B');
            } else if (s.rfind("namespace", 0) == 0) {
                const std::string name = m.str(1);
                if (!inNs) {
                    const bool fuse = name == "fuse" || name.rfind("fuse::", 0) == 0;
                    anyFuse |= fuse;
                    const bool ok = fuse || name == "std" || (!header && name.empty());
                    if (!ok && !inlineAllowed(lines[li].raw, "namespace")) {
                        v.push_back(where(f, root, li) + ": top-level namespace '" + (name.empty() ? "<anonymous>" : name) +
                                    "' outside fuse::");
                    }
                }
                stack.push_back('N');
            } else if (s.rfind("extern", 0) == 0) {
                stack.push_back('E');
            } else {
                const bool qualified = m[5].matched; // struct std::hash<...> specialisation
                const bool global = std::none_of(stack.begin(), stack.end(), [](char k) { return k != 'E'; });
                if (header && global && !qualified && !inlineAllowed(lines[li].raw, "namespace")) {
                    v.push_back(where(f, root, li) + ": " + m.str(3) + " '" + m.str(4) + "' declared at global scope");
                }
                stack.push_back('T');
            }
        }
        (void)anyFuse;
    }
    if (headers < 20u) {
        v.push_back(root.generic_string() + ": only " + std::to_string(headers) + " public headers found (wrong --root?)");
    }
    return v;
}

// ---- check: macros ----------------------------------------------------------------------------------

Violations checkMacros(const fs::path& root) {
    Violations v;
    const std::regex def(R"(^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*))");
    const std::regex torqueAssert(R"(\b(AssertFatal|AssertWarn|AssertISV|AssertRelease|TORQUE_UNUSED)\b)");
    std::set<std::string> coreDefined;
    const auto headers = productFiles(root, true);
    for (const fs::path& f : headers) {
        const std::string r = "/" + rel(f, root);
        const auto lines = scanLines(readFile(f));
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (!std::regex_search(lines[i].code, m, def)) {
                continue;
            }
            const std::string name = m.str(1);
            if (r.rfind("/Core/include/", 0) == 0) {
                coreDefined.insert(name);
            }
            // new_ban.hpp redefines malloc/free/new/delete on purpose (B1.3 debug heap intercept).
            if (name.rfind("FUSE_", 0) != 0 && !r.ends_with("/fuse/alloc/new_ban.hpp") &&
                !inlineAllowed(lines[i].raw, "macros")) {
                v.push_back(where(f, root, i) + ": public macro '" + name + "' is not FUSE_-prefixed");
            }
        }
    }
    for (const char* required : {"FUSE_ASSERT", "FUSE_HOST_DEVICE", "FUSE_VERIFY"}) {
        if (!coreDefined.count(required)) {
            v.push_back(std::string("Core/include: required macro ") + required + " is not defined by fuse_core public headers");
        }
    }
    for (const fs::path& f : productFiles(root, false)) {
        const auto lines = scanLines(readFile(f));
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i].bare, m, torqueAssert)) {
                v.push_back(where(f, root, i) + ": Torque macro '" + m.str(1) + "' (use FUSE_ASSERT / FUSE_VERIFY)");
            }
        }
    }
    return v;
}

// ---- check: torque-macros ---------------------------------------------------------------------------

bool isTextFile(const fs::path& p) {
    if (isCxxSource(p) || hasExt(p, {".cmake", ".txt", ".glsl", ".hlsl", ".comp", ".vert", ".frag", ".json", ".in", ".py", ".sh", ".md", ".toml", ".yaml", ".yml", ".ts", ".cs"})) {
        return true;
    }
    return p.filename() == "CMakeLists.txt";
}

Violations checkTorqueMacros(const fs::path& root) {
    Violations v;
    const std::regex torque(R"(\bTORQUE_[A-Z0-9_]*)");
    for (const fs::path& f : listFiles(root, [](const fs::path& p, const std::string& r) {
             return !inQuarantine(r) && isTextFile(p) && !hasExt(p, {".md"});
         })) {
        const auto lines = scanLines(readFile(f));
        const bool cmake = hasExt(f, {".cmake", ".txt"});
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            // Comments may mention TORQUE_* when documenting the compat mapping; code may not.
            const std::string& text = cmake ? lines[i].raw.substr(0, lines[i].raw.find('#')) : lines[i].code;
            if (std::regex_search(text, m, torque) && !inlineAllowed(lines[i].raw, "torque-macros")) {
                v.push_back(where(f, root, i) + ": '" + m.str(0) + "' outside Compat/ Legacy/ quarantine");
            }
        }
    }
    return v;
}

// ---- check: torque-names ----------------------------------------------------------------------------

Violations checkTorqueNames(const fs::path& root) {
    Violations v;
    const std::regex torqueish(R"(torque|t3d|t2d|\btge\b|\btgea\b)");
    const std::regex enumChannel(R"(\benum\s+(class|struct)?\s*(Channel|LogChannel|Domain|MemoryDomain|AllocDomain|MemTag|MemoryTag)\b)");
    const std::regex channelRef(R"(\b(Channel|LogChannel|MemoryDomain|AllocDomain|MemTag|MemoryTag)::([A-Za-z0-9_]+))");
    const std::regex domainCtor(R"re(\b(DomainBudget|MemoryDomain|AllocDomain|ProfilerDomain|LogChannel)\b[^;]*"([^"]*)")re");
    const std::regex logTag(R"re(\bFUSE_LOG_[A-Z_]+\s*\([^"]*"\s*\[([^\]]*)\])re");
    for (const fs::path& f : productFiles(root, false)) {
        const auto lines = scanLines(readFile(f));
        int enumDepth = -1; // brace depth while inside a channel/domain enum body
        int depth = 0;
        bool pendingEnum = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& bare = lines[i].bare;
            const std::string& code = lines[i].code;
            std::smatch m;
            if (std::regex_search(bare, enumChannel)) {
                pendingEnum = true;
            }
            for (char c : bare) {
                if (c == '{') {
                    if (pendingEnum) {
                        enumDepth = depth;
                        pendingEnum = false;
                    }
                    ++depth;
                } else if (c == '}') {
                    --depth;
                    if (depth == enumDepth) {
                        enumDepth = -1;
                    }
                }
            }
            std::string hit;
            if ((enumDepth >= 0 || pendingEnum) && std::regex_search(lower(bare), torqueish)) {
                hit = "log channel / memory domain enumerator";
            }
            for (auto it = std::sregex_iterator(bare.begin(), bare.end(), channelRef); hit.empty() && it != std::sregex_iterator(); ++it) {
                if (std::regex_search(lower((*it).str(2)), torqueish)) {
                    hit = "channel/domain '" + (*it).str(0) + "'";
                }
            }
            if (hit.empty() && std::regex_search(code, m, domainCtor) && std::regex_search(lower(m.str(2)), torqueish)) {
                hit = "memory domain / channel name \"" + m.str(2) + "\"";
            }
            if (hit.empty() && std::regex_search(code, m, logTag) && std::regex_search(lower(m.str(1)), torqueish)) {
                hit = "log tag [" + m.str(1) + "]";
            }
            if (!hit.empty() && !inlineAllowed(lines[i].raw, "torque-names")) {
                v.push_back(where(f, root, i) + ": Torque-named " + hit);
            }
        }
    }
    return v;
}

// ---- manifest ---------------------------------------------------------------------------------------

struct TargetRow {
    std::string name, type, sourceDir, cxxStandard;
    bool hasCxx = false, hasCuda = false;
    std::string links, interfaceLinks;
};

bool readManifest(const fs::path& p, std::vector<TargetRow>& rows, Violations& v) {
    std::ifstream in(p);
    if (!in) {
        v.push_back(p.generic_string() + ": target manifest missing (re-run CMake configure)");
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '|')) {
            f.push_back(cell);
        }
        f.resize(8);
        rows.push_back({f[0], f[1], f[2], f[3], f[4] == "1", f[5] == "1", f[6], f[7]});
    }
    return true;
}

// ---- check: banned-deps -----------------------------------------------------------------------------

// Narrow ImGui allow-list (Meridian stays banned everywhere). FUSE Relight is injected into D3D8/D3D9
// games, where a Qt overlay is impossible; docs/plans/FUSE_REMIX_PORT_PLAN.md AD-14 and §3 row 36 adopt
// vendored upstream Dear ImGui for the in-game developer menu in Relight/overlay (RL-6.x), and
// Relight/THIRD_PARTY.md must name it for attribution (§0.4.1). Paths are relative to Source/FUSE.
bool imguiAllowedPath(const std::string& relPath) {
    return relPath == "Relight/THIRD_PARTY.md" || relPath.rfind("Relight/overlay/", 0) == 0;
}

bool imguiAllowedTarget(const std::string& sourceDir) {
    return (sourceDir + "/").find("/Source/FUSE/Relight/overlay/") != std::string::npos;
}

Violations checkBannedDeps(const fs::path& root, const fs::path& manifest) {
    Violations v;
    const std::regex banned(R"(meridian|imgui)");
    const std::regex meridianOnly(R"(meridian)");
    for (const fs::path& f : listFiles(root, [](const fs::path&, const std::string&) { return true; })) {
        const std::string r = rel(f, root);
        const std::regex& bannedHere = imguiAllowedPath(r) ? meridianOnly : banned;
        if (std::regex_search(lower(r), bannedHere)) {
            v.push_back(r + ": banned name in path");
            continue;
        }
        if (!isTextFile(f)) {
            continue;
        }
        const auto lines = scanLines(readFile(f));
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            const std::string l = lower(lines[i].raw);
            if (std::regex_search(l, m, bannedHere)) {
                v.push_back(where(f, root, i) + ": banned dependency '" + m.str(0) + "'");
            }
        }
    }
    std::vector<TargetRow> rows;
    if (readManifest(manifest, rows, v)) {
        for (const TargetRow& t : rows) {
            const std::string links = lower(t.links + "," + t.interfaceLinks);
            std::smatch m;
            if (std::regex_search(links, m, imguiAllowedTarget(t.sourceDir) ? meridianOnly : banned)) {
                v.push_back("target " + t.name + ": links banned '" + m.str(0) + "' (" + t.links + ")");
            }
        }
    }
    return v;
}

// ---- check: editor-qt6 --------------------------------------------------------------------------------

struct EditorQtScan {
    Violations v;
    bool configured = false; ///< fuse_editor present in the manifest
    size_t closureTargets = 0;
    size_t hostFiles = 0;
    std::vector<std::string> qtLinks;
};

/// `$<LINK_ONLY:Qt6::Gui>` -> `Qt6::Gui`; plain names unchanged.
std::string stripGenex(std::string t) {
    while (t.rfind("$<", 0) == 0) {
        const size_t colon = t.find(':');
        if (colon == std::string::npos) {
            break;
        }
        t = t.substr(colon + 1);
        while (!t.empty() && t.back() == '>') {
            t.pop_back();
        }
    }
    const auto b = t.find_first_not_of(" \t");
    const auto e = t.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
}

EditorQtScan checkEditorQt6(const fs::path& root, const fs::path& manifest, size_t minHostFiles) {
    EditorQtScan out;
    std::vector<TargetRow> rows;
    if (!readManifest(manifest, rows, out.v)) {
        return out;
    }
    std::map<std::string, const TargetRow*> byName;
    for (const TargetRow& r : rows) {
        byName[r.name] = &r;
    }
    if (!byName.count("fuse_editor")) {
        return out; // not configured: caller skips
    }
    out.configured = true;

    // Transitive link closure of the editor executable over the fuse_* target graph.
    std::set<std::string> seenTargets;
    std::set<std::string> leaves;
    std::vector<std::string> stack{"fuse_editor"};
    while (!stack.empty()) {
        const std::string name = stack.back();
        stack.pop_back();
        if (!seenTargets.insert(name).second) {
            continue;
        }
        const TargetRow* row = byName[name];
        for (const std::string* list : {&row->links, &row->interfaceLinks}) {
            std::stringstream ss(*list);
            std::string cell;
            while (std::getline(ss, cell, ',')) {
                const std::string dep = stripGenex(cell);
                if (dep.empty()) {
                    continue;
                }
                if (byName.count(dep)) {
                    stack.push_back(dep);
                } else {
                    leaves.insert(dep);
                }
            }
        }
    }
    out.closureTargets = seenTargets.size();
    const std::regex imgui(R"(imgui)", std::regex::icase);
    const std::regex anyQt(R"(^Qt([0-9]*)::)");
    bool widgets6 = false;
    for (const std::string& dep : leaves) {
        std::smatch m;
        if (std::regex_search(dep, imgui)) {
            out.v.push_back("fuse_editor link closure contains ImGui ('" + dep + "')");
        }
        if (std::regex_search(dep, m, anyQt)) {
            out.qtLinks.push_back(dep);
            if (m.str(1) != "6") {
                out.v.push_back("fuse_editor link closure contains non-Qt6 Qt target '" + dep + "' (Qt 6 only)");
            }
            widgets6 = widgets6 || dep == "Qt6::Widgets";
        }
    }
    for (const std::string& t : seenTargets) {
        if (std::regex_search(t, imgui)) {
            out.v.push_back("fuse_editor link closure contains ImGui target '" + t + "'");
        }
    }
    if (!widgets6) {
        out.v.push_back("fuse_editor does not link Qt6::Widgets (editor chrome must be Qt 6 widgets)");
    }

    // Includes across the editor module.
    const fs::path editor = root / "Editor";
    const std::regex include(R"(^\s*#\s*include\s*([<"])([^>"]+)[>"])");
    const std::regex qtHeader(R"(^(Q[A-Za-z0-9_]*|Qt[A-Za-z0-9_]*/.*)$)");
    for (const fs::path& f : listFiles(editor, [](const fs::path& p, const std::string&) { return isCxxSource(p); })) {
        const std::string r = rel(f, editor);
        const std::string stem = f.stem().string();
        const bool qtHost = r.find("/qt/") != std::string::npos || r.rfind("qt/", 0) == 0 ||
                            (stem.size() > 3 && stem.compare(stem.size() - 3, 3, "_qt") == 0);
        const bool qtHostDir = r.rfind("src/qt/", 0) == 0;
        out.hostFiles += qtHostDir ? 1u : 0u;
        const auto lines = scanLines(readFile(f));
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (!std::regex_search(lines[i].code, m, include)) {
                continue;
            }
            const std::string header = m.str(2);
            const bool angle = m.str(1) == "<";
            if (std::regex_search(header, imgui)) {
                out.v.push_back(where(f, root, i) + ": ImGui include <" + header + "> in the editor");
                continue;
            }
            const bool isQt = angle && std::regex_match(header, qtHeader);
            if (isQt && !qtHost) {
                out.v.push_back(where(f, root, i) + ": Qt include <" + header + "> outside the Qt host (src/qt/, *_qt.*)");
            }
            if (qtHostDir && angle && !isQt) {
                const bool fuseApi = header.rfind("fuse/", 0) == 0;
                const bool vulkan = header.rfind("vulkan/", 0) == 0;
                const bool stdOrC = header.find('/') == std::string::npos;
                if (!fuseApi && !vulkan && !stdOrC) {
                    out.v.push_back(where(f, root, i) + ": Qt host includes <" + header +
                                    "> (only FUSE public headers, Qt, Vulkan and std allowed)");
                }
            }
        }
    }
    if (out.hostFiles < minHostFiles) {
        out.v.push_back(editor.generic_string() + ": only " + std::to_string(out.hostFiles) +
                        " Qt host sources under src/qt/ (wrong --root?)");
    }
    return out;
}

// ---- check: qt-includes -----------------------------------------------------------------------------

Violations checkQtIncludes(const fs::path& root, const fs::path& manifest) {
    Violations v;
    const std::regex qtInclude(R"(^\s*#\s*include\s*[<"](Q[A-Za-z0-9_]*|Qt[A-Za-z0-9_]*/[^>"]*)[>"])");
    const std::regex qtLink(R"((^|,)\s*(\$<[^>]*:)?Qt[0-9]*(::|Core|Gui|Widgets|$))");
    size_t scanned = 0;
    for (const char* mod : {"Core", "Renderer", "Physics", "ECS", "Compute"}) {
        const fs::path dir = root / mod;
        for (const fs::path& f : listFiles(dir, [](const fs::path& p, const std::string&) { return isCxxSource(p); })) {
            ++scanned;
            const auto lines = scanLines(readFile(f));
            for (size_t i = 0; i < lines.size(); ++i) {
                std::smatch m;
                if (std::regex_search(lines[i].code, m, qtInclude)) {
                    v.push_back(where(f, root, i) + ": Qt include <" + m.str(1) + "> in " + mod);
                }
            }
        }
    }
    if (scanned < 20u) {
        v.push_back(root.generic_string() + ": too few sources scanned (wrong --root?)");
    }
    std::vector<TargetRow> rows;
    if (readManifest(manifest, rows, v)) {
        const std::set<std::string> guarded = {"fuse_core", "fuse_rhi", "fuse_renderer", "fuse_physics", "fuse_ecs", "fuse_compute"};
        size_t seen = 0;
        for (const TargetRow& t : rows) {
            if (!guarded.count(t.name)) {
                continue;
            }
            ++seen;
            if (std::regex_search(t.links, qtLink) || std::regex_search(t.interfaceLinks, qtLink)) {
                v.push_back("target " + t.name + ": links Qt (" + t.links + ")");
            }
        }
        if (seen == 0u) {
            v.push_back("manifest lists none of fuse_core/fuse_rhi/fuse_physics/fuse_ecs/fuse_compute");
        }
    }
    return v;
}

// ---- check: cxx-standard ----------------------------------------------------------------------------

Violations checkCxxStandard(const fs::path& manifest, const fs::path& repo) {
    Violations v;
    std::vector<TargetRow> rows;
    if (!readManifest(manifest, rows, v)) {
        return v;
    }
    const std::string repoGen = repo.generic_string();
    size_t checked = 0;
    for (const TargetRow& t : rows) {
        if (t.type == "INTERFACE_LIBRARY" || t.type == "UTILITY" || !t.hasCxx) {
            continue; // header-only, custom, C-only, or CUDA-only targets are not C++ host targets
        }
        std::string r = t.sourceDir;
        if (!repoGen.empty() && r.rfind(repoGen, 0) == 0) {
            r = r.substr(repoGen.size());
        }
        // Torque quarantine is pinned to C++17 by design (Source/FUSE/CMakeLists.txt, FuseCxx23.cmake);
        // Engine/lib/* are vendored third-party C/C++ libraries built under fuse_* names.
        if (r.rfind("/Source/FUSE/Legacy", 0) == 0 || r.rfind("/Engine/", 0) == 0) {
            continue;
        }
        ++checked;
        if (t.cxxStandard != "23") {
            v.push_back("target " + t.name + " (" + r + "): CXX_STANDARD='" + t.cxxStandard + "', expected 23");
        }
    }
    if (checked < 10u) {
        v.push_back(manifest.generic_string() + ": only " + std::to_string(checked) + " fuse_* host targets in manifest");
    }
    return v;
}

// ---- check: doc-headings ----------------------------------------------------------------------------

Violations checkDocHeadings(const fs::path& plan, const fs::path& sources) {
    Violations v;
    const std::regex bHead(R"(^###\s+B([0-9]+)\.([0-9]+)\b)");
    const std::regex srcHead(R"(^##\s+([0-9]+)\.([0-9]+)(?![0-9.]))");
    std::map<std::string, std::set<std::string>> have; // phase -> {"N.M"}
    std::ifstream in(plan);
    if (!in) {
        return {plan.generic_string() + ": master plan missing"};
    }
    std::string line;
    size_t lineNo = 0, headings = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::smatch m;
        if (!std::regex_search(line, m, bHead)) {
            continue;
        }
        ++headings;
        const std::string phase = m.str(1);
        const std::string nm = phase + "." + m.str(2);
        if (!have.count(phase)) {
            auto& set = have[phase];
            std::ifstream src(sources / ("P" + phase + ".md"));
            std::string s;
            while (std::getline(src, s)) {
                std::smatch sm;
                if (std::regex_search(s, sm, srcHead)) {
                    set.insert(sm.str(1) + "." + sm.str(2));
                }
            }
        }
        if (!have[phase].count(nm)) {
            v.push_back(rel(plan, plan.parent_path()) + ":" + std::to_string(lineNo) + ": '### B" + nm +
                        "' has no '## " + nm + "' in " + (sources / ("P" + phase + ".md")).filename().string());
        }
    }
    if (headings < 10u) {
        v.push_back(plan.generic_string() + ": only " + std::to_string(headings) + " '### B*.*' headings found");
    }
    return v;
}

// ---- check: vendored-pins ---------------------------------------------------------------------------

/// SHA-256 (FIPS 180-4) of `data`, lowercase hex.
std::string sha256Hex(const std::string& data) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    const std::uint64_t bitLen = std::uint64_t(data.size()) * 8u;
    msg.push_back(char(0x80));
    while (msg.size() % 64u != 56u) {
        msg.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(char((bitLen >> (i * 8)) & 0xffu));
    }
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < msg.size(); off += 64u) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = std::uint32_t(std::uint8_t(msg[off + i * 4])) << 24 | std::uint32_t(std::uint8_t(msg[off + i * 4 + 1])) << 16 |
                   std::uint32_t(std::uint8_t(msg[off + i * 4 + 2])) << 8 | std::uint32_t(std::uint8_t(msg[off + i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::array<std::uint32_t, 8> v = h;
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t t2 = S0 + maj;
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (int i = 0; i < 8; ++i) {
            h[i] += v[i];
        }
    }
    std::string hex;
    char buf[9];
    for (std::uint32_t x : h) {
        std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(x));
        hex += buf;
    }
    return hex;
}

std::string withoutCr(const std::string& text) {
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

Violations checkVendoredPins(const fs::path& dir) {
    Violations v;
    const fs::path pinPath = dir / "VERSION";
    const std::string where = pinPath.generic_string();
    if (!fs::is_regular_file(pinPath)) {
        v.push_back(where + ": missing pin file");
        return v;
    }
    std::map<std::string, std::string> kv;
    std::vector<std::pair<std::string, std::string>> hashes; // relative path -> expected sha256
    std::istringstream in(withoutCr(readFile(pinPath)));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            v.push_back(where + ": malformed line '" + line + "'");
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key.rfind("sha256:", 0) == 0) {
            hashes.emplace_back(key.substr(7), value);
        } else {
            kv[key] = value;
        }
    }
    for (const char* key : {"name", "upstream", "version", "tag", "commit", "license", "license_file", "version_header"}) {
        if (kv[key].empty()) {
            v.push_back(where + ": required field '" + std::string(key) + "' missing or empty");
        }
    }
    const std::string& version = kv["version"];
    if (!std::regex_match(version, std::regex(R"([0-9]+\.[0-9]+\.[0-9]+)"))) {
        v.push_back(where + ": version '" + version + "' is not X.Y.Z");
    }
    if (!std::regex_match(kv["commit"], std::regex("[0-9a-f]{40}"))) {
        v.push_back(where + ": commit '" + kv["commit"] + "' is not a full 40-hex commit hash");
    }
    if (!version.empty() && kv["tag"] != version && kv["tag"] != "v" + version) {
        v.push_back(where + ": tag '" + kv["tag"] + "' does not name version " + version);
    }
    if (kv["upstream"].rfind("https://", 0) != 0) {
        v.push_back(where + ": upstream '" + kv["upstream"] + "' is not an https URL");
    }
    if (!kv["license_file"].empty()) {
        const fs::path license = dir / kv["license_file"];
        if (!fs::is_regular_file(license) || readFile(license).find("Copyright") == std::string::npos) {
            v.push_back(license.generic_string() + ": license file missing or has no copyright notice");
        }
    }

    // The version the vendored header declares must be the pinned one.
    const std::string& headerRel = kv["version_header"];
    if (!headerRel.empty()) {
        const fs::path header = dir / headerRel;
        if (!fs::is_regular_file(header)) {
            v.push_back(header.generic_string() + ": version_header missing");
        } else {
            const std::string text = readFile(header);
            const std::regex makeVersion(R"(#define\s+\w*VERSION\s+\(\s*VK_MAKE_VERSION\(\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*\)\s*\))");
            const std::regex docVersion(R"(<b>Version ([0-9]+\.[0-9]+\.[0-9]+)</b>)");
            // FidelityFX SDK style: #define FFX_<EFFECT>_VERSION_MAJOR (1) / _MINOR / _PATCH.
            const std::regex partVersion(R"(#define\s+(\w+)_VERSION_(MAJOR|MINOR|PATCH)\s+\(?\s*([0-9]+)\s*\)?)");
            // NVIDIA Image Scaling style banner: "NVIDIA Image Scaling SDK  - v1.0.3".
            const std::regex sdkBanner(R"(SDK\s+-\s+v([0-9]+\.[0-9]+\.[0-9]+))");
            std::vector<std::string> declared;
            for (std::sregex_iterator it(text.begin(), text.end(), makeVersion), end; it != end; ++it) {
                declared.push_back((*it).str(1) + "." + (*it).str(2) + "." + (*it).str(3));
            }
            for (std::sregex_iterator it(text.begin(), text.end(), docVersion), end; it != end; ++it) {
                declared.push_back((*it).str(1));
            }
            std::map<std::string, std::array<std::string, 3>> parts; // prefix -> {major, minor, patch}
            for (std::sregex_iterator it(text.begin(), text.end(), partVersion), end; it != end; ++it) {
                const std::string which = (*it).str(2);
                parts[(*it).str(1)][which == "MAJOR" ? 0 : (which == "MINOR" ? 1 : 2)] = (*it).str(3);
            }
            for (const auto& [prefix, mmp] : parts) {
                if (!mmp[0].empty() && !mmp[1].empty() && !mmp[2].empty()) {
                    declared.push_back(mmp[0] + "." + mmp[1] + "." + mmp[2]);
                }
            }
            for (std::sregex_iterator it(text.begin(), text.end(), sdkBanner), end; it != end; ++it) {
                declared.push_back((*it).str(1));
            }
            if (declared.empty()) {
                v.push_back(header.generic_string() +
                            ": declares no version (VK_MAKE_VERSION define, <b>Version X.Y.Z</b>, *_VERSION_MAJOR/MINOR/PATCH "
                            "defines or an 'SDK - vX.Y.Z' banner)");
            }
            // `header_version` pins the component version a multi-component SDK header declares when it
            // differs from the SDK tag (e.g. FSR1 1.2.0 inside FidelityFX SDK v1.1.4).
            const std::string expectedDeclared = kv["header_version"].empty() ? version : kv["header_version"];
            if (!kv["header_version"].empty() &&
                !std::regex_match(kv["header_version"], std::regex(R"([0-9]+\.[0-9]+\.[0-9]+)"))) {
                v.push_back(where + ": header_version '" + kv["header_version"] + "' is not X.Y.Z");
            }
            for (const std::string& d : declared) {
                if (d != expectedDeclared) {
                    v.push_back(header.generic_string() + ": declares version " + d + ", pin says " + expectedDeclared);
                }
            }
        }
    }

    // Vendored bytes match the pin (CRLF-normalized, so a text=auto checkout still verifies).
    bool headerHashed = false;
    for (const auto& [relPath, expected] : hashes) {
        headerHashed = headerHashed || relPath == headerRel;
        const fs::path file = dir / relPath;
        if (!fs::is_regular_file(file)) {
            v.push_back(file.generic_string() + ": pinned file missing");
            continue;
        }
        const std::string actual = sha256Hex(withoutCr(readFile(file)));
        if (actual != expected) {
            v.push_back(file.generic_string() + ": sha256 " + actual + " != pinned " + expected);
        }
    }
    if (!headerRel.empty() && !headerHashed) {
        v.push_back(where + ": no sha256:" + headerRel + " entry");
    }
    return v;
}

// ---- check: branding --------------------------------------------------------------------------------
// Appendix A "Icons, installer, docs, CI badge names" (docs + CI half; the icon / installer half is
// fuse_package_gate). Over --repo:
//   * every .github/workflows/*.yml has a top-level `name:` that says FUSE, and no workflow / job /
//     step `name:` uses Torque / T3D product naming (badges and the Actions UI show these names);
//   * README.md opens with `# FUSE`, carries one CI badge per workflow whose alt text is exactly the
//     workflow's `name:` and whose target is that workflow file (…/actions/workflows/<file>/badge.svg);
//   * docs/README.md's title says FUSE and no top-level docs/*.md title names Torque / T3D.

bool mentionsTorque(const std::string& s) {
    static const std::regex re(R"(torque|t3d)", std::regex::icase);
    return std::regex_search(s, re);
}

std::string unquoteYaml(std::string v) {
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == ',')) v.pop_back();
    const size_t hash = v.find(" #");
    if (hash != std::string::npos && v.find_first_of("\"'") == std::string::npos) v.resize(hash);
    while (!v.empty() && v.back() == ' ') v.pop_back();
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
}

std::string firstHeading(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# ", 0) == 0) return line.substr(2);
    }
    return {};
}

Violations checkBranding(const fs::path& repo) {
    Violations v;
    const fs::path wfDir = repo / ".github/workflows";
    std::map<std::string, std::string> workflows; // file name -> top-level name
    if (fs::is_directory(wfDir)) {
        for (const auto& e : fs::directory_iterator(wfDir)) {
            if (!e.is_regular_file() || !hasExt(e.path(), {".yml", ".yaml"})) continue;
            const std::string file = e.path().filename().string();
            const std::string shown = ".github/workflows/" + file;
            std::istringstream in(readFile(e.path()));
            std::string line, top;
            bool haveTop = false;
            int ln = 0;
            static const std::regex nameRe(R"(^(\s*)(?:-\s*)?(?:\{\s*)?name:\s*(.*)$)");
            while (std::getline(in, line)) {
                ++ln;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::smatch m;
                if (!std::regex_match(line, m, nameRe)) continue;
                const std::string val = unquoteYaml(m[2].str());
                if (m[1].length() == 0 && !haveTop) {
                    haveTop = true;
                    top = val;
                }
                if (mentionsTorque(val)) {
                    v.push_back(shown + ":" + std::to_string(ln) + ": name '" + val + "' uses Torque/T3D product naming");
                }
            }
            if (!haveTop) {
                v.push_back(shown + ": no top-level `name:` (badges would show the file name)");
            } else if (top.find("FUSE") == std::string::npos) {
                v.push_back(shown + ": workflow name '" + top + "' does not say FUSE");
            }
            workflows[file] = top;
        }
    }
    if (workflows.empty()) {
        v.push_back(wfDir.generic_string() + ": no workflows found");
    }

    const fs::path readme = repo / "README.md";
    const std::string text = fs::exists(readme) ? readFile(readme) : std::string();
    if (text.empty()) {
        v.push_back("README.md: missing");
    } else {
        std::istringstream in(text);
        std::string first;
        while (std::getline(in, first) && (first.empty() || first == "\r")) {}
        if (!first.empty() && first.back() == '\r') first.pop_back();
        if (first != "# FUSE" && first.rfind("# FUSE ", 0) != 0) {
            v.push_back("README.md: title '" + first + "' is not '# FUSE'");
        }
        static const std::regex badgeRe(R"(\[!\[([^\]]*)\]\(([^)\s]*/actions/workflows/([^/)\s]+)/badge\.svg[^)\s]*)\)\]\(([^)\s]*)\))");
        std::set<std::string> badged;
        for (std::sregex_iterator it(text.begin(), text.end(), badgeRe), end; it != end; ++it) {
            const std::string alt = (*it)[1].str();
            const std::string file = (*it)[3].str();
            const std::string link = (*it)[4].str();
            badged.insert(file);
            auto wf = workflows.find(file);
            if (wf == workflows.end()) {
                v.push_back("README.md: badge '" + alt + "' points at missing workflow " + file);
                continue;
            }
            if (alt != wf->second) {
                v.push_back("README.md: badge alt '" + alt + "' != workflow name '" + wf->second + "' (" + file + ")");
            }
            if (alt.find("FUSE") == std::string::npos || mentionsTorque(alt)) {
                v.push_back("README.md: badge '" + alt + "' does not use FUSE naming");
            }
            if (link.find("/actions/workflows/" + file) == std::string::npos) {
                v.push_back("README.md: badge '" + alt + "' links to '" + link + "', not its workflow runs");
            }
        }
        for (const auto& [file, name] : workflows) {
            if (!badged.count(file)) {
                v.push_back("README.md: no CI badge for workflow " + file + " ('" + name + "')");
            }
        }
    }

    const fs::path docs = repo / "docs";
    const fs::path docsReadme = docs / "README.md";
    if (!fs::exists(docsReadme)) {
        v.push_back("docs/README.md: missing");
    } else if (firstHeading(readFile(docsReadme)).find("FUSE") == std::string::npos) {
        v.push_back("docs/README.md: title '" + firstHeading(readFile(docsReadme)) + "' does not say FUSE");
    }
    if (fs::is_directory(docs)) {
        for (const auto& e : fs::directory_iterator(docs)) {
            if (!e.is_regular_file() || !hasExt(e.path(), {".md"})) continue;
            const std::string h = firstHeading(readFile(e.path()));
            if (mentionsTorque(h)) {
                v.push_back("docs/" + e.path().filename().string() + ": title '" + h + "' uses Torque/T3D product naming");
            }
        }
    }
    return v;
}

// ---- self-tests -------------------------------------------------------------------------------------

struct SelfTest {
    bool ok = true;
    void expect(bool cond, const std::string& what) {
        if (!cond) {
            std::fprintf(stderr, "SELFTEST FAIL: %s\n", what.c_str());
            ok = false;
        }
    }
};

size_t countContaining(const Violations& v, const std::string& needle) {
    return size_t(std::count_if(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; }));
}

/// Writes a fake Source/FUSE tree under `base` from {relative path, contents} pairs.
fs::path seedTree(const fs::path& base, std::initializer_list<std::pair<const char*, const char*>> files) {
    std::error_code ec;
    fs::remove_all(base, ec);
    for (const auto& [path, text] : files) {
        writeFile(base / path, text);
    }
    return base;
}

std::string padHeaders(const fs::path& base, int n) {
    // Some checks require a minimum number of headers so a wrong --root cannot pass silently.
    for (int i = 0; i < n; ++i) {
        writeFile(base / "Core/include/fuse/pad" / ("pad" + std::to_string(i) + ".hpp"),
                  "#pragma once\nnamespace fuse::pad {\nstruct P" + std::to_string(i) + " {};\n}\n");
        writeFile(base / "Physics/src/pad" / ("pad" + std::to_string(i) + ".cpp"), "namespace fuse {}\n");
    }
    return {};
}

bool selfTest(const std::string& check, const fs::path& scratch) {
    SelfTest t;
    const fs::path bad = scratch / "selftest_bad";
    const fs::path good = scratch / "selftest_good";
    if (check == "ownership") {
        seedTree(bad, {{"include/fuse/w.hpp",
                        "#pragma once\n"
                        "struct Widget;\n"
                        "Widget* createWidget(int id);\n"
                        "inline Widget* mk() {\n"
                        "    return new Widget(1);\n"
                        "}\n"
                        "inline void kill(Widget* w) { delete w; }\n"
                        "struct Holder { Widget* release(); };\n"}});
        const Violations vb = checkOwnership(bad);
        t.expect(vb.size() == 4u, "ownership: seeded factory, new, delete and release() all flagged (got " + std::to_string(vb.size()) + ")");
        seedTree(good, {{"include/fuse/w.hpp",
                         "#pragma once\n"
                         "struct Snapshot; struct Foo { Foo(const Foo&) = delete; };\n"
                         "const Snapshot* newest() const;\n"
                         "// Widget* createWidget(int id);\n"
                         "/* return new Widget(1);\n   delete w; */\n"
                         "inline const char* s = \"new Widget(\";\n"
                         "Widget* openPooled(); // fuse-lint-allow(ownership): non-owning view into the pool\n"
                         "#define FUSE_NEW(T) new T()\n"},
                        {"include/fuse/fuse_lint_selftest_allow.hpp", "#pragma once\nWidget* openView();\n"},
                        {"include/fuse/alloc/new_ban.hpp", "#define new if (0) {} else new\nvoid f() { delete p; }\n"},
                        {"tests/include/x.hpp", "Widget* createWidget();\n"}});
        const Violations vg = checkOwnership(good);
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "ownership: comments, strings, deleted ctors, allowlisted views and new_ban.hpp pass");
    } else if (check == "namespace") {
        seedTree(bad, {{"Core/include/fuse/a.hpp", "#pragma once\nnamespace torque {\nstruct X {};\n}\n"},
                       {"Core/include/fuse/b.hpp", "#pragma once\nstruct GlobalThing {\n  int x;\n};\n"},
                       {"Core/include/fuse/c.hpp", "#pragma once\nnamespace {\nint hidden;\n}\n"},
                       {"Core/src/d.cpp", "namespace engine {\nvoid f() {}\n}\n"},
                       {"Compat/include/compat/ok.hpp", "namespace Con { struct Legacy {}; }\n"}});
        padHeaders(bad, 20);
        const Violations vb = checkNamespace(bad);
        t.expect(countContaining(vb, "'torque'") == 1u, "namespace: seeded top-level namespace torque flagged");
        t.expect(countContaining(vb, "'GlobalThing'") == 1u, "namespace: seeded global struct flagged");
        t.expect(countContaining(vb, "<anonymous>") == 1u, "namespace: anonymous namespace in public header flagged");
        t.expect(countContaining(vb, "'engine'") == 1u, "namespace: seeded source namespace engine flagged");
        t.expect(vb.size() == 4u, "namespace: Compat/ quarantine ignored (got " + std::to_string(vb.size()) + ")");
        seedTree(good, {{"Core/include/fuse/a.hpp",
                         "#pragma once\nnamespace fuse::core {\nnamespace detail {\nstruct X { struct Y {}; };\n}\n"
                         "enum class E { A };\n}\n"
                         "template <> struct std::hash<fuse::core::detail::X> { int operator()() const { return 0; } };\n"
                         "namespace std {\ntemplate <> struct hash<int*> {};\n}\n"
                         "extern \"C\" {\nvoid fuse_c_api();\n}\n"
                         "// namespace torque {\n#define FUSE_X struct Bad {}\n"},
                        {"Core/src/b.cpp", "namespace {\nstruct Local {};\n}\nnamespace fuse {\nvoid f() { const char* s = \"namespace q {\"; }\n}\n"}});
        padHeaders(good, 20);
        const Violations vg = checkNamespace(good);
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "namespace: nested/detail/std specialisations/extern C/strings pass");
    } else if (check == "macros") {
        seedTree(bad, {{"Core/include/fuse/assert.hpp", "#pragma once\n#define ASSERT(x) (void)(x)\n#define FUSE_VERIFY(c, m) (void)(c)\n"},
                       {"Physics/src/p.cpp", "void f() { AssertFatal(false, \"x\"); }\n"}});
        padHeaders(bad, 1);
        const Violations vb = checkMacros(bad);
        t.expect(countContaining(vb, "'ASSERT'") == 1u, "macros: seeded non-FUSE_ public macro flagged");
        t.expect(countContaining(vb, "FUSE_ASSERT is not defined") == 1u, "macros: missing FUSE_ASSERT flagged");
        t.expect(countContaining(vb, "FUSE_HOST_DEVICE is not defined") == 1u, "macros: missing FUSE_HOST_DEVICE flagged");
        t.expect(countContaining(vb, "AssertFatal") == 1u, "macros: Torque AssertFatal usage flagged");
        seedTree(good, {{"Core/include/fuse/assert.hpp", "#pragma once\n#define FUSE_ASSERT(c, m) (void)(c)\n#define FUSE_VERIFY(c, m) (void)(c)\n"},
                        {"Core/include/fuse/gria.hpp", "#pragma once\n#define FUSE_HOST_DEVICE\n"},
                        {"Core/include/fuse/alloc/new_ban.hpp", "#define new if (0) {} else new\n"},
                        {"Legacy/include/t.hpp", "#define AssertFatal(x, y)\n"},
                        {"Physics/src/p.cpp", "// AssertFatal was replaced\nvoid f() { const char* s = \"AssertFatal\"; }\n"}});
        const Violations vg = checkMacros(good);
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "macros: FUSE_* macros, new_ban.hpp and Legacy/ quarantine pass");
    } else if (check == "torque-macros") {
        seedTree(bad, {{"Core/src/a.cpp", "#ifdef TORQUE_DEBUG\n#endif\n"},
                       {"Core/CMakeLists.txt", "target_compile_definitions(x PRIVATE TORQUE_SHIPPING=1)\n"}});
        const Violations vb = checkTorqueMacros(bad);
        t.expect(vb.size() == 2u, "torque-macros: seeded TORQUE_* in source and CMake flagged (got " + std::to_string(vb.size()) + ")");
        seedTree(good, {{"Core/src/a.cpp", "// maps TORQUE_DEBUG to FUSE_DEBUG\nconst char* s = \"x\";\n"},
                        {"Core/CMakeLists.txt", "# TORQUE_SHIPPING lives in Compat\n"},
                        {"Compat/src/c.cpp", "#ifdef TORQUE_DEBUG\n#endif\n"},
                        {"Legacy/T3D/CMakeLists.txt", "add_definitions(-DTORQUE_OS_LINUX)\n"}});
        const Violations vg = checkTorqueMacros(good);
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "torque-macros: comments and Compat/ Legacy/ quarantine pass");
    } else if (check == "torque-names") {
        seedTree(bad, {{"Core/include/fuse/log/logger.hpp", "namespace fuse::log {\nenum class Channel : unsigned {\n    Core = 1,\n    TorqueScript = 2,\n};\n}\n"},
                       {"Core/src/a.cpp", "void f() {\n  fuse::alloc::DomainBudget b(\"torque_legacy\", 64u);\n  log(Channel::T3DBridge, \"x\");\n  FUSE_LOG_INFO(Channel::Core, \"[Torque] hello\");\n}\n"}});
        const Violations vb = checkTorqueNames(bad);
        t.expect(vb.size() == 4u, "torque-names: seeded enumerator, domain name, channel ref and log tag flagged (got " + std::to_string(vb.size()) + ")");
        seedTree(good, {{"Core/include/fuse/log/logger.hpp", "namespace fuse::log {\nenum class Channel : unsigned {\n    Core = 1,\n    Script = 2,\n};\n}\n"},
                        {"Core/src/a.cpp", "void f() {\n  fuse::alloc::DomainBudget b(\"script\", 64u);\n  // Channel::TorqueScript was renamed\n  FUSE_LOG_INFO(Channel::Core, \"[compat] loaded torque mission\");\n}\n"},
                        {"Compat/src/c.cpp", "DomainBudget b(\"torque\", 1);\n"}});
        const Violations vg = checkTorqueNames(good);
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "torque-names: FUSE names, comments and Compat/ quarantine pass");
    } else if (check == "banned-deps") {
        seedTree(bad, {{"Editor/CMakeLists.txt", "target_link_libraries(fuse_editor PRIVATE imgui)\n"},
                       {"Editor/src/meridian_panel.cpp", "int x;\n"},
                       {"Legacy/src/x.cpp", "#include \"Meridian/api.h\"\n"},
                       // Relight allow-list is narrow: ImGui outside overlay/ is still flagged, and
                       // Meridian is flagged even inside overlay/.
                       {"Relight/render/menu.cpp", "#include <imgui.h>\n"},
                       {"Relight/overlay/m.cpp", "#include \"Meridian/api.h\"\n"}});
        writeFile(scratch / "bad.manifest", "fuse_editor|EXECUTABLE|/x|23|1|0|fuse_core,imgui::imgui|\n"
                                            "fuse_relight_render|STATIC_LIBRARY|/r/Source/FUSE/Relight/render|23|1|0|imgui|\n");
        const Violations vb = checkBannedDeps(bad, scratch / "bad.manifest");
        t.expect(vb.size() == 7u, "banned-deps: seeded ImGui CMake link, Meridian path/include, imgui target links and "
                                  "Relight ImGui outside overlay/ flagged (got " + std::to_string(vb.size()) + ")");
        seedTree(good, {{"Editor/CMakeLists.txt", "target_link_libraries(fuse_editor PRIVATE Qt6::Widgets)\n"},
                        {"Relight/overlay/dev_menu.cpp", "#include <imgui.h>\n"},
                        {"Relight/THIRD_PARTY.md", "| Dear ImGui | MIT | Engine/lib/imgui |\n"}});
        writeFile(scratch / "good.manifest", "fuse_editor|EXECUTABLE|/x|23|1|0|fuse_core,Qt6::Widgets|\n"
                                             "fuse_relight_overlay|STATIC_LIBRARY|/r/Source/FUSE/Relight/overlay|23|1|0|imgui|\n");
        const Violations vg = checkBannedDeps(good, scratch / "good.manifest");
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "banned-deps: clean tree and Qt-only editor links pass");
    } else if (check == "editor-qt6") {
        seedTree(bad, {{"Editor/src/qt/w.cpp", "#include <imgui.h>\n#include <console/console.h>\n#include <QWidget>\n"},
                       {"Editor/src/panel.cpp", "#include <QWidget>\n"}});
        writeFile(scratch / "bad.manifest",
                  "fuse_editor|EXECUTABLE|/x|23|1|0|fuse_editor_qt|\n"
                  "fuse_editor_qt|STATIC_LIBRARY|/x|23|1|0|fuse_editor_api,Qt5::Widgets,imgui_backend|\n"
                  "fuse_editor_api|STATIC_LIBRARY|/x|23|1|0|fuse_core|\n");
        const EditorQtScan sb = checkEditorQt6(bad, scratch / "bad.manifest", 1);
        for (const auto& s : sb.v) {
            std::fprintf(stderr, "  seeded: %s\n", s.c_str());
        }
        t.expect(sb.configured && sb.v.size() == 6u,
                 "editor-qt6: seeded ImGui include, Torque include, Qt outside host, Qt5 link, ImGui link and "
                 "missing Qt6::Widgets flagged (got " + std::to_string(sb.v.size()) + ")");
        seedTree(good, {{"Editor/src/qt/a.cpp", "#include \"a.hpp\"\n#include <fuse/editor/editor_host.hpp>\n#include <QWidget>\n#include <QtCore/QObject>\n#include <vector>\n#include <vulkan/vulkan.h>\n#include <unistd.h>\n"},
                        {"Editor/src/qt/b.cpp", "// #include <imgui.h>\n#include <QMenu>\n"},
                        {"Editor/src/viewport_vulkan_surface_qt.cpp", "#include <QVulkanInstance>\n"},
                        {"Editor/src/panel.cpp", "#include <fuse/editor/editor_host.hpp>\n#include <queue>\n"}});
        writeFile(scratch / "good.manifest",
                  "fuse_editor|EXECUTABLE|/x|23|1|0|fuse_editor_qt|\n"
                  "fuse_editor_qt|STATIC_LIBRARY|/x|23|1|0|fuse_editor_api,Qt6::Widgets,Qt6::Gui|fuse_editor_api,Qt6::Widgets,$<LINK_ONLY:Qt6::Gui>\n"
                  "fuse_editor_api|STATIC_LIBRARY|/x|23|1|0|fuse_core,Qt6::Gui|fuse_core,$<LINK_ONLY:Qt6::Gui>\n"
                  "fuse_core|STATIC_LIBRARY|/x|23|1|0|Threads::Threads|\n");
        const EditorQtScan sg = checkEditorQt6(good, scratch / "good.manifest", 2);
        for (const auto& s : sg.v) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(sg.configured && sg.v.empty() && sg.closureTargets == 4u,
                 "editor-qt6: Qt6-only closure, host-scoped Qt includes and FUSE/std/Vulkan includes pass");
        writeFile(scratch / "none.manifest", "fuse_core|STATIC_LIBRARY|/x|23|1|0||\n");
        const EditorQtScan sn = checkEditorQt6(good, scratch / "none.manifest", 2);
        t.expect(!sn.configured && sn.v.empty(), "editor-qt6: manifest without fuse_editor reports not-configured");
    } else if (check == "qt-includes") {
        seedTree(bad, {{"Core/src/a.cpp", "#include <QString>\n"},
                       {"Renderer/include/fuse/r.hpp", "#  include <QtGui/QWindow>\n"},
                       {"Physics/src/p.cpp", "#include \"QObject\"\n"}});
        padHeaders(bad, 20);
        writeFile(scratch / "bad.manifest", "fuse_core|STATIC_LIBRARY|/x|23|1|0|Qt6::Core|\nfuse_rhi|STATIC_LIBRARY|/x|23|1|0|fuse_core|Qt6::Gui\n");
        const Violations vb = checkQtIncludes(bad, scratch / "bad.manifest");
        t.expect(vb.size() == 5u, "qt-includes: seeded Qt includes and Qt links flagged (got " + std::to_string(vb.size()) + ")");
        seedTree(good, {{"Core/src/a.cpp", "// #include <QString>\n#include <queue>\n#include \"quat.hpp\"\n"},
                        {"Editor/src/e.cpp", "#include <QWidget>\n"}});
        padHeaders(good, 20);
        writeFile(scratch / "good.manifest", "fuse_core|STATIC_LIBRARY|/x|23|1|0|fuse_quat,Threads::Threads|\nfuse_editor|EXECUTABLE|/x|23|1|0|Qt6::Widgets|\n");
        const Violations vg = checkQtIncludes(good, scratch / "good.manifest");
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "qt-includes: std includes, comments and fuse_editor Qt use pass");
    } else if (check == "cxx-standard") {
        std::string rows = "# header\n";
        for (int i = 0; i < 10; ++i) {
            rows += "fuse_ok" + std::to_string(i) + "|STATIC_LIBRARY|/r/Source/FUSE/Core|23|1|0||\n";
        }
        const std::string goodRows = rows +
                                     "fuse_legacy|STATIC_LIBRARY|/r/Source/FUSE/Legacy/T3D|17|1|0||\n"
                                     "fuse_lua|STATIC_LIBRARY|/r/Engine/lib/lua||0|0||\n"
                                     "fuse_cuda_kernels|STATIC_LIBRARY|/r/Source/FUSE/Compute||0|1||\n"
                                     "fuse_headers|INTERFACE_LIBRARY|/r/Source/FUSE/Core||0|0||\n";
        writeFile(scratch / "bad.manifest", rows + "fuse_bad17|STATIC_LIBRARY|/r/Source/FUSE/ECS|17|1|0||\nfuse_unset|EXECUTABLE|/r/Tools/FUSE||1|0||\n");
        const Violations vb = checkCxxStandard(scratch / "bad.manifest", "/r");
        t.expect(vb.size() == 2u, "cxx-standard: seeded C++17 and unset targets flagged (got " + std::to_string(vb.size()) + ")");
        writeFile(scratch / "good.manifest", goodRows);
        const Violations vg = checkCxxStandard(scratch / "good.manifest", "/r");
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "cxx-standard: Legacy quarantine, vendored C, CUDA-only and interface targets exempt");
    } else if (check == "doc-headings") {
        std::string plan = "# Plan\n### Banned / gated\n";
        std::string p1 = "# P1\n";
        for (int i = 1; i <= 10; ++i) {
            plan += "### B1." + std::to_string(i) + " — Section\n";
            p1 += "## 1." + std::to_string(i) + " — Section\n";
        }
        writeFile(good / "plan.md", plan);
        writeFile(good / "sources/P1.md", p1);
        writeFile(bad / "plan.md", plan + "### B1.11 — Missing\n### B2.1 — No source file\n");
        writeFile(bad / "sources/P1.md", p1 + "## 1.110 — Not a match\n### 1.11 — Wrong level\n");
        const Violations vb = checkDocHeadings(bad / "plan.md", bad / "sources");
        t.expect(vb.size() == 2u, "doc-headings: seeded B1.11 and B2.1 without source sections flagged (got " + std::to_string(vb.size()) + ")");
        const Violations vg = checkDocHeadings(good / "plan.md", good / "sources");
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "doc-headings: matching plan/sources pass");
    } else if (check == "vendored-pins") {
        t.expect(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 "vendored-pins: sha256(\"abc\") known answer");
        t.expect(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 "vendored-pins: sha256(\"\") known answer");
        const std::string header = "// lib\n#define LIB_VERSION (VK_MAKE_VERSION(1, 2, 3))\n/* <b>Version 1.2.3</b> */\n";
        const std::string license = "Copyright (c) 2020 Someone\nPermission is hereby granted...\n";
        auto pin = [&](const std::string& version, const std::string& commit, const std::string& headerSha) {
            return "# pin\nname=lib\nupstream=https://example.com/lib\nversion=" + version + "\ntag=v" + version +
                   "\ncommit=" + commit + "\nlicense=MIT\nlicense_file=LICENSE.txt\nversion_header=include/lib.h\n" +
                   "sha256:include/lib.h=" + headerSha + "\nsha256:LICENSE.txt=" + sha256Hex(license) + "\n";
        };
        const std::string commit(40, 'a');
        seedTree(good, {});
        writeFile(good / "include/lib.h", header);
        writeFile(good / "LICENSE.txt", license);
        writeFile(good / "VERSION", pin("1.2.3", commit, sha256Hex(header)));
        const Violations vg = checkVendoredPins(good);
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "vendored-pins: consistent pin passes");
        // CRLF checkout of the same bytes still verifies.
        std::string crlf;
        for (char c : header) {
            if (c == '\n') crlf += '\r';
            crlf += c;
        }
        writeFile(good / "include/lib.h", crlf);
        t.expect(checkVendoredPins(good).empty(), "vendored-pins: CRLF checkout of pinned header passes");
        // Bad: pin says 1.2.4 (header declares 1.2.3 twice -> 2), short commit (1), header edited
        // after pinning (sha mismatch, 1), license file missing (license check + pinned hash, 2) => 6.
        seedTree(bad, {});
        writeFile(bad / "include/lib.h", header);
        writeFile(bad / "VERSION", pin("1.2.4", "abc123", sha256Hex(header + "// local edit\n")));
        const Violations vb = checkVendoredPins(bad);
        t.expect(vb.size() == 6u && countContaining(vb, "declares version 1.2.3") == 2u &&
                     countContaining(vb, "40-hex") == 1u && countContaining(vb, "sha256") >= 1u,
                 "vendored-pins: seeded version/commit/hash/license violations flagged (got " + std::to_string(vb.size()) + ")");
        t.expect(checkVendoredPins(bad / "nonexistent").size() == 1u, "vendored-pins: missing pin file flagged");
        // Multi-component SDK: tag v1.1.4, header declares its component version via MAJOR/MINOR/PATCH
        // defines, pinned by header_version; a banner-style header ("SDK  - v1.0.3") pins the tag directly.
        const std::string sdkHeader = "#define FFX_FOO_VERSION_MAJOR      (1)\n#define FFX_FOO_VERSION_MINOR (2)\n"
                                      "#define FFX_FOO_VERSION_PATCH      (0)\n";
        const fs::path sdk = good / "sdk";
        writeFile(sdk / "include/lib.h", sdkHeader);
        writeFile(sdk / "LICENSE.txt", license);
        writeFile(sdk / "VERSION", pin("1.1.4", commit, sha256Hex(sdkHeader)) + "header_version=1.2.0\n");
        t.expect(checkVendoredPins(sdk).empty(), "vendored-pins: MAJOR/MINOR/PATCH header + header_version passes");
        writeFile(sdk / "VERSION", pin("1.1.4", commit, sha256Hex(sdkHeader)) + "header_version=1.3.0\n");
        t.expect(countContaining(checkVendoredPins(sdk), "declares version 1.2.0") == 1u,
                 "vendored-pins: header_version mismatch flagged");
        const std::string banner = "// NVIDIA Image Scaling SDK  - v1.0.3\n";
        writeFile(sdk / "include/lib.h", banner);
        writeFile(sdk / "VERSION", pin("1.0.3", commit, sha256Hex(banner)));
        t.expect(checkVendoredPins(sdk).empty(), "vendored-pins: 'SDK - vX.Y.Z' banner header passes");
    } else if (check == "branding") {
        const char* wfGood =
            "name: FUSE Umbrella (Linux)\non:\n  push:\njobs:\n  a:\n    name: fuse_core + tests\n    steps:\n      - name: Build\n";
        const char* readmeGood =
            "# FUSE\n\n[![FUSE Umbrella (Linux)](https://github.com/o/r/actions/workflows/umbrella.yml/badge.svg?branch=main)]"
            "(https://github.com/o/r/actions/workflows/umbrella.yml)\n"
            "[![FUSE Nightly](https://github.com/o/r/actions/workflows/nightly.yml/badge.svg)](https://github.com/o/r/actions/workflows/nightly.yml)\n\n"
            "Heritage: forked from Torque3D.\n";
        seedTree(good, {{".github/workflows/umbrella.yml", wfGood},
                        {".github/workflows/nightly.yml", "name: \"FUSE Nightly\"  \non:\n  schedule:\njobs:\n  b:\n    strategy:\n      matrix:\n        config:\n          - { name: \"Ubuntu GCC\", cc: gcc }\n"},
                        {"README.md", readmeGood},
                        {"docs/README.md", "# FUSE documentation\n"},
                        {"docs/heritage.md", "# Heritage\n\nFUSE descends from Torque3D.\n"}});
        const Violations vg = checkBranding(good);
        for (const auto& s : vg) {
            std::fprintf(stderr, "  unexpected: %s\n", s.c_str());
        }
        t.expect(vg.empty(), "branding: FUSE-named workflows, README title/badges and docs titles pass");
        // Bad: workflow name without FUSE (1), Torque job name (1), missing top-level name (1),
        // README title (1), badge to missing workflow (1), badge alt != name + not FUSE (2),
        // unbadged workflows x3 (3), docs/README title (1), Torque docs title (1) => 12.
        seedTree(bad, {{".github/workflows/linux.yml", "name: Linux Build\njobs:\n  a:\n    name: Torque3D unit tests\n"},
                       {".github/workflows/noname.yml", "on: push\njobs:\n  a:\n    runs-on: x\n"},
                       {".github/workflows/win.yml", "name: FUSE Windows\n"},
                       {"README.md", "# Torque3D\n\n[![Build](https://github.com/o/r/actions/workflows/gone.yml/badge.svg)](https://github.com/o/r/actions/workflows/gone.yml)\n"
                                     "[![Windows](https://github.com/o/r/actions/workflows/win.yml/badge.svg)](https://github.com/o/r/actions/workflows/win.yml)\n"},
                       {"docs/README.md", "# Engine documentation\n"},
                       {"docs/t3d.md", "# T3D reference\n"}});
        const Violations vb = checkBranding(bad);
        for (const auto& s : vb) {
            std::fprintf(stderr, "  seeded: %s\n", s.c_str());
        }
        t.expect(vb.size() == 11u && countContaining(vb, "does not say FUSE") == 2u && countContaining(vb, "Torque/T3D") == 2u &&
                     countContaining(vb, "no top-level") == 1u && countContaining(vb, "is not '# FUSE'") == 1u &&
                     countContaining(vb, "missing workflow") == 1u && countContaining(vb, "no CI badge") == 2u,
                 "branding: seeded workflow/README/docs naming violations flagged (got " + std::to_string(vb.size()) + ")");
        t.expect(checkBranding(bad / "nonexistent").size() >= 2u, "branding: missing repo flagged");
    } else if (check == "asset-licences" || check == "asset-credits") {
        t.expect(fuse::tools::lint::selfTestAssetLicences(scratch, sha256Hex),
                 "asset-licences: seeded licence / sha / provenance / credits violations flagged, clean lock passes");
    } else {
        std::fprintf(stderr, "unknown check '%s'\n", check.c_str());
        return false;
    }
    std::error_code ec;
    fs::remove_all(bad, ec);
    fs::remove_all(good, ec);
    return t.ok;
}

int usage() {
    std::fprintf(stderr,
                 "usage: fuse_lint <ownership|namespace|macros|torque-macros|torque-names|banned-deps|cxx-standard|"
                 "qt-includes|editor-qt6|doc-headings|vendored-pins|branding|asset-licences|asset-credits> [--root DIR] [--dir DIR] "
                 "[--manifest FILE] [--repo DIR] [--plan FILE] [--sources DIR] [--cache DIR] --scratch DIR\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    Args a;
    a.check = argv[1];
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const fs::path val = argv[i + 1];
        if (k == "--root") a.root = val;
        else if (k == "--dir") a.dir = val;
        else if (k == "--manifest") a.manifest = val;
        else if (k == "--repo") a.repo = val;
        else if (k == "--plan") a.plan = val;
        else if (k == "--sources") a.sources = val;
        else if (k == "--scratch") a.scratch = val;
        else if (k == "--cache") a.cache = val;
        else return usage();
    }
    if (a.scratch.empty()) {
        a.scratch = fs::temp_directory_path() / ("fuse_lint_" + a.check);
    }
    if (!selfTest(a.check, a.scratch)) {
        std::fprintf(stderr, "fuse_lint %s: self-test FAILED (the check cannot detect its seeded violation)\n", a.check.c_str());
        return 2;
    }
    std::printf("fuse_lint %s: self-test ok (seeded violations detected, clean sample passes)\n", a.check.c_str());

    Violations v;
    fs::path shown;
    if (a.check == "ownership") v = checkOwnership(shown = a.dir);
    else if (a.check == "namespace") v = checkNamespace(shown = a.root);
    else if (a.check == "macros") v = checkMacros(shown = a.root);
    else if (a.check == "torque-macros") v = checkTorqueMacros(shown = a.root);
    else if (a.check == "torque-names") v = checkTorqueNames(shown = a.root);
    else if (a.check == "banned-deps") v = checkBannedDeps(shown = a.root, a.manifest);
    else if (a.check == "qt-includes") v = checkQtIncludes(shown = a.root, a.manifest);
    else if (a.check == "editor-qt6") {
        const EditorQtScan scan = checkEditorQt6(shown = a.root, a.manifest, 5);
        if (!scan.configured && scan.v.empty()) {
            std::printf("SKIP fuse_lint editor-qt6: fuse_editor not configured (Qt6 not found or FUSE_BUILD_EDITOR=OFF)\n");
            return 77;
        }
        std::string qt;
        for (const std::string& l : scan.qtLinks) {
            qt += (qt.empty() ? "" : ", ") + l;
        }
        std::printf("fuse_editor link closure: %zu fuse_* targets, Qt links: %s; %zu Qt host sources scanned\n",
                    scan.closureTargets, qt.c_str(), scan.hostFiles);
        v = scan.v;
    }
    else if (a.check == "cxx-standard") v = checkCxxStandard(shown = a.manifest, a.repo);
    else if (a.check == "doc-headings") v = checkDocHeadings(shown = a.plan, a.sources);
    else if (a.check == "vendored-pins") v = checkVendoredPins(shown = a.dir);
    else if (a.check == "branding") v = checkBranding(shown = a.repo);
    else if (a.check == "asset-licences") v = fuse::tools::lint::checkAssetLicences(shown = a.root, a.manifest, a.cache, sha256Hex);
    else if (a.check == "asset-credits") {
        std::string credits, error;
        if (!fuse::tools::lint::renderAssetCredits(a.root / "licences.lock.json", credits, error)) {
            std::fprintf(stderr, "fuse_lint asset-credits: %s\n", error.c_str());
            return 1;
        }
        writeFile(a.root / "CREDITS.md", credits);
        std::printf("fuse_lint asset-credits: wrote %s\n", (a.root / "CREDITS.md").generic_string().c_str());
        return 0;
    }
    else return usage();

    for (const std::string& s : v) {
        std::fprintf(stderr, "  %s\n", s.c_str());
    }
    std::printf("fuse_lint %s over %s: %zu violation(s)\n", a.check.c_str(), shown.generic_string().c_str(), v.size());
    return v.empty() ? 0 : 1;
}
