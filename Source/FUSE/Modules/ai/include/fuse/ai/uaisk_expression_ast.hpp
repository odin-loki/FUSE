#pragma once

#include <fuse/types.hpp>

#include <string>
#include <string_view>

namespace fuse::ai::uaisk {

enum class UaiskExpressionOp : u8 {
    Literal = 0,
    Less,
    Greater,
    Equal,
};

/// Lightweight condition expression AST for UAISK `.cs` field values (e.g. `distance < 10`).
struct UaiskExpressionAst {
    UaiskExpressionOp op = UaiskExpressionOp::Literal;
    std::string fieldName;
    float threshold = 0.f;
    bool valid = false;
};

/// Parse a simple comparison expression (`field < value`, `field > value`, `field == value`).
[[nodiscard]] bool parseExpressionAst(std::string_view text, UaiskExpressionAst& outExpr);

} // namespace fuse::ai::uaisk
