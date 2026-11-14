// TorqueScript static analysis (plan §1.2 Track B, W1.6): per-file function definitions, call sites and object
// declarations (datablock / singleton / named file-scope `new`), and the project-wide aggregation into a call
// graph (definitions with file:line, packages, unresolved callees) and a datablock tree (children linked to
// their copy-source parent by case-insensitive name, missing parents reported). All outputs are sorted so
// they do not depend on the order files were added.
#pragma once

#include "fuse/remaster/tscript/parser.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fuse::remaster::tscript {

enum class CallKind : std::uint8_t {
    Plain,     // foo(...)
    Namespace, // Ns::foo(...)
    Parent,    // Parent::foo(...)
    Method,    // <receiver>.foo(...)   callee recorded as ".foo"
};
[[nodiscard]] std::string_view call_kind_name(CallKind k) noexcept;

struct FunctionDef {
    std::string key;     // "fn" or "Ns::fn" as written
    std::string package; // enclosing package, empty if none
    std::vector<std::string> params;
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
};

struct CallSite {
    std::string caller; // function key, or "<file>" for file-scope code
    std::string callee; // "fn" / "Ns::fn" / "parent::fn" (unresolved parent) / ".method"
    CallKind kind = CallKind::Plain;
    bool resolved = true;  // false for a Parent call that could not be resolved statically
    std::string receiver;  // Method: literal object name if the receiver is a bare word; Parent: enclosing ns
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
};

struct ObjectField {
    std::string name;
    std::string index; // source text of the [index] list, empty if none
    std::string value; // source text of the value expression
};

struct ObjectDef {
    std::string kind;     // "datablock" / "singleton" / "new"
    std::string cls;      // class name (source text if computed)
    std::string name;     // object name, empty if anonymous
    std::string parent;   // copy-source (`: Parent`), empty if none
    std::string function; // enclosing function key, empty at file scope
    std::string container; // name of the enclosing object declaration (nested `new`), empty if none
    std::vector<ObjectField> fields;
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    [[nodiscard]] bool file_scope() const noexcept { return function.empty(); }
};

struct FileAnalysis {
    std::string file;
    std::vector<FunctionDef> functions;
    std::vector<CallSite> calls;
    std::vector<ObjectDef> objects; // every datablock / singleton, plus named `new` objects at file scope
    std::vector<std::string> packages;
};

[[nodiscard]] FileAnalysis analyze(const ParseResult& parsed);

// ---- project-wide aggregation ----

struct CallGraph {
    // lower-cased key -> definitions (a function may be defined in several files / packages)
    std::map<std::string, std::vector<FunctionDef>> definitions;
    std::vector<CallSite> edges;                               // sorted (file, line, col, caller, callee)
    std::map<std::string, std::vector<std::string>> packages;   // package -> function keys (sorted)
    std::map<std::string, std::uint32_t> unresolved;           // Plain/Namespace callee (lower) not defined -> sites
    std::uint32_t unresolved_parent_calls = 0;
};

struct DatablockNode {
    ObjectDef def;
    int parent_index = -1;     // index into DatablockTree::nodes, -1 if no parent or parent missing
    std::vector<int> children; // indices, sorted
};

struct MissingParent {
    std::string child;
    std::string parent;
    std::string file;
    std::uint32_t line = 0;
};

struct DatablockTree {
    std::vector<DatablockNode> nodes; // sorted (file, line, col)
    std::vector<int> roots;           // nodes without a (found) parent
    std::vector<MissingParent> missing_parents;
    std::uint32_t duplicate_names = 0; // names declared more than once (later ones still link to the first)
};

[[nodiscard]] CallGraph build_call_graph(const std::vector<FileAnalysis>& files);
[[nodiscard]] DatablockTree build_datablock_tree(const std::vector<FileAnalysis>& files);

[[nodiscard]] std::string to_lower(std::string_view s);

} // namespace fuse::remaster::tscript
