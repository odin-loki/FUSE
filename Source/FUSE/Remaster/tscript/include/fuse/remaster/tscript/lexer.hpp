// TorqueScript lexer (plan §1.2 Track B, W1.6). Mirrors Torque3D's CMDscan.l closely enough for the
// template scripts: // and /* */ comments, "strings" and 'tagged strings' with backslash escapes, ints
// (decimal / 0x hex) and floats (.5, 1.5e3), identifiers joined across `::` (Foo::bar), %local and $global
// variables (with `::` segments), and the operator set (incl. $= !$= --> ->). Keywords stay identifiers; the
// parser treats them contextually, which is what lets `datablock = "x";` work as a field assignment.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::remaster::tscript {

struct Diagnostic {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::string message;
};

enum class TokKind : std::uint8_t { Ident, LocalVar, GlobalVar, Int, Float, String, Tagged, Punct, End };

struct Token {
    TokKind kind = TokKind::End;
    std::string_view text; // identifier / operator / literal; for strings the raw contents without quotes
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::uint32_t begin = 0; // byte range of the whole token in the source (quotes included)
    std::uint32_t end = 0;
};

// Tokenises `source` (a leading UTF-8 BOM is skipped). Always ends with one End token. Lexical errors are
// appended to `diags` (with `file`) and the offending character is skipped.
[[nodiscard]] std::vector<Token> lex(std::string_view source, std::string_view file, std::vector<Diagnostic>& diags);

} // namespace fuse::remaster::tscript
