// TorqueScript parser: tokens -> AST (plan §1.2 Track B, W1.6). Recursive descent for statements, precedence
// climbing for expressions with Torque3D's CMDgram.y precedence (assignment < ?: < || < && < | < ^ < & <
// == != < relational < @ SPC TAB NL $= !$= < << >> < + - < * / % < unary < postfix . [] --> ->). Errors are
// reported with line:col and the parser recovers by skipping to the next `;` / `}` at the same brace depth.
#pragma once

#include "fuse/remaster/tscript/ast.hpp"
#include "fuse/remaster/tscript/lexer.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::remaster::tscript {

struct ParseResult {
    std::string file;   // as given (used in diagnostics and analysis output)
    std::string source; // the parsed text (owned so analyses can slice node ranges)
    NodePtr root;       // Kind::Script, never null
    std::vector<Diagnostic> diags;

    [[nodiscard]] bool ok() const noexcept { return diags.empty(); }
    // Source text of a node's [begin, end) range.
    [[nodiscard]] std::string_view slice(const Node& n) const;
};

[[nodiscard]] ParseResult parse(std::string source, std::string file = "<input>");

// Reads and parses a file; a read failure is reported as a diagnostic at 0:0. `display_name` (if non-empty)
// replaces the path in diagnostics / analysis output (e.g. a repo-relative path for deterministic dumps).
[[nodiscard]] ParseResult parse_file(const std::filesystem::path& path, std::string display_name = {});

} // namespace fuse::remaster::tscript
