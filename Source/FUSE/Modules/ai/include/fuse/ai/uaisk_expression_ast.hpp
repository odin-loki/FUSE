#pragma once

#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::ai::uaisk {

enum class UaiskExpressionOp : u8 {
    Literal = 0,
    Less,
    Greater,
    Equal,
    And,
    Or,
    Not,
};

struct UaiskExpressionEvalContext {
    f32 distance = 0.f;
    f32 patrolRadius = 0.f;
    f32 allyRadius = 0.f;
};

/// Lightweight condition expression AST for UAISK `.cs` field values
/// (e.g. `distance < 10`, `(distance < 12 && patrolRadius > 5)`).
struct UaiskExpressionAst {
    UaiskExpressionOp op = UaiskExpressionOp::Literal;
    std::string fieldName;
    f32 threshold = 0.f;
    bool valid = false;
    std::vector<UaiskExpressionAst> children;
};

/// Parse a comparison, boolean, or nested parenthesized expression.
[[nodiscard]] bool parseExpressionAst(std::string_view text, UaiskExpressionAst& outExpr);

/// Evaluate a parsed expression against a lightweight blackboard context.
[[nodiscard]] bool evaluateExpressionAst(const UaiskExpressionAst& expr, const UaiskExpressionEvalContext& ctx);

/// Collect leaf comparison nodes (used by codegen field-default wiring).
void collectExpressionLeaves(const UaiskExpressionAst& expr, std::vector<UaiskExpressionAst>& outLeaves);

} // namespace fuse::ai::uaisk
