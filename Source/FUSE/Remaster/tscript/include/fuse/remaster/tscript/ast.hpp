// TorqueScript AST (docs/plans/FUSE_REMASTER_PLAN.md §1.2 Track B, task W1.6 `fuse_remaster_tscript`).
//
// One generic node type: a kind, a text payload (identifier / operator / literal), the source location
// (1-based line and byte column, plus the [begin, end) byte range so analyses can slice the original source)
// and an ordered list of children. Child layout per kind is documented on the enum below; an absent optional
// child is a `None` node so positions stay fixed.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::remaster::tscript {

enum class Kind : std::uint8_t {
    // ---- structure ----
    Script,       // kids: top-level statements / declarations
    FunctionDecl, // text: qualified name ("fn" / "Ns::fn"); kids: [List(params), Block]
    Param,        // text: "%name"; kids: [default expr] (optional, Torque 4 default args)
    PackageDecl,  // text: package name; kids: FunctionDecl...
    List,         // generic ordered list; text says which ("params", "args", "index", "values", "body")
    None,         // placeholder for an absent optional child
    // ---- statements ----
    Block,    // kids: statements
    If,       // kids: [cond, then, else|None]
    While,    // kids: [cond, body]
    DoWhile,  // kids: [body, cond]
    For,      // kids: [init|None, cond|None, step|None, body]
    Foreach,  // text: "foreach" / "foreach$"; kids: [LocalVar, collection, body]
    Switch,   // text: "switch" / "switch$"; kids: [subject, Case...]
    Case,     // text: "case" / "default"; kids: [List(values), Block]
    Break,    //
    Continue, //
    Return,   // kids: [value] (optional)
    ExprStmt, // kids: [expr]
    Empty,    // a lone ';'
    // ---- objects ----
    ObjectDecl,  // text: "new" / "singleton" / "datablock"; kids: [class, name|None, copy-source|None,
                 //       List(args), List(body)|None]; body items are FieldAssign / ObjectDecl
    FieldAssign, // text: field name; kids: [List(index), value]
    // ---- expressions ----
    Int,          // text: literal as written (decimal or 0x hex)
    Float,        // text: literal as written
    String,       // text: raw contents between the quotes (escapes not decoded)
    TaggedString, // text: raw contents between the single quotes
    Bool,         // text: "true" / "false"
    Ident,        // text: bare word (object name / string-like constant), may be "A::b" qualified
    LocalVar,     // text: "%name"
    GlobalVar,    // text: "$name" / "$pref::Video::mode"
    Index,        // kids: [base, index exprs...]           base[a, b]
    Field,        // text: field name; kids: [object]        obj.field
    InternalName, // text: "-->name" / "->name"; kids: [object]
    Call,         // text: callee ("fn", "Ns::fn", "Parent::fn"); kids: args
    MethodCall,   // text: method name; kids: [receiver, args...]
    Unary,        // text: "!" "~" "-"; kids: [operand]
    PreIncDec,    // text: "++" / "--"; kids: [operand]
    PostIncDec,   // text: "++" / "--"; kids: [operand]
    Binary,       // text: operator ("+", "@", "SPC", "$=", ...); kids: [lhs, rhs]
    Assign,       // text: "=", "+=", ...; kids: [target, value]
    Ternary,      // kids: [cond, then, else]
};

[[nodiscard]] std::string_view kind_name(Kind k) noexcept;

struct Node {
    Kind kind = Kind::None;
    std::string text;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::uint32_t begin = 0; // byte offset of the first token
    std::uint32_t end = 0;   // byte offset one past the last token
    std::vector<std::unique_ptr<Node>> kids;

    [[nodiscard]] const Node& kid(std::size_t i) const { return *kids[i]; }
    [[nodiscard]] bool is(Kind k) const noexcept { return kind == k; }
};

using NodePtr = std::unique_ptr<Node>;

} // namespace fuse::remaster::tscript
