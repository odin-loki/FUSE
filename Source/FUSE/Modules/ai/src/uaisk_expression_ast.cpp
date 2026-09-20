#include <fuse/ai/uaisk_expression_ast.hpp>

#include <cstdlib>

namespace fuse::ai::uaisk {

namespace {

std::string_view trimView(std::string_view input) {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t' || input.front() == '"')) {
        input.remove_prefix(1);
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t' || input.back() == '"' ||
                              input.back() == ';' || input.back() == 'f')) {
        input.remove_suffix(1);
    }
    return input;
}

f32 parseLiteral(std::string_view text) {
    text = trimView(text);
    if (text.empty()) {
        return 0.f;
    }
    try {
        return std::stof(std::string(text));
    } catch (...) {
        return 0.f;
    }
}

f32 fieldValue(const UaiskExpressionEvalContext& ctx, std::string_view fieldName) {
    if (fieldName == "distance" || fieldName == "distanceThreshold" || fieldName == "patrolDistance") {
        return ctx.distance;
    }
    if (fieldName == "patrolRadius") {
        return ctx.patrolRadius;
    }
    if (fieldName == "allyRadius") {
        return ctx.allyRadius;
    }
    return 0.f;
}

bool parseComparison(std::string_view text, UaiskExpressionAst& outExpr);
bool parsePrimary(std::string_view text, UaiskExpressionAst& outExpr);
bool parseUnary(std::string_view text, UaiskExpressionAst& outExpr);
bool parseAnd(std::string_view text, UaiskExpressionAst& outExpr);
bool parseOr(std::string_view text, UaiskExpressionAst& outExpr);

bool parseComparison(std::string_view text, UaiskExpressionAst& outExpr) {
    outExpr = UaiskExpressionAst{};
    text = trimView(text);
    if (text.empty()) {
        return false;
    }

    const std::size_t eqPos = text.find("==");
    if (eqPos != std::string_view::npos) {
        outExpr.op = UaiskExpressionOp::Equal;
        outExpr.fieldName = std::string(trimView(text.substr(0, eqPos)));
        outExpr.threshold = parseLiteral(text.substr(eqPos + 2));
        outExpr.valid = !outExpr.fieldName.empty();
        return outExpr.valid;
    }

    const std::size_t ltPos = text.find('<');
    if (ltPos != std::string_view::npos) {
        outExpr.op = UaiskExpressionOp::Less;
        outExpr.fieldName = std::string(trimView(text.substr(0, ltPos)));
        outExpr.threshold = parseLiteral(text.substr(ltPos + 1));
        outExpr.valid = !outExpr.fieldName.empty();
        return outExpr.valid;
    }

    const std::size_t gtPos = text.find('>');
    if (gtPos != std::string_view::npos) {
        outExpr.op = UaiskExpressionOp::Greater;
        outExpr.fieldName = std::string(trimView(text.substr(0, gtPos)));
        outExpr.threshold = parseLiteral(text.substr(gtPos + 1));
        outExpr.valid = !outExpr.fieldName.empty();
        return outExpr.valid;
    }

    outExpr.op = UaiskExpressionOp::Literal;
    outExpr.threshold = parseLiteral(text);
    outExpr.valid = true;
    return true;
}

bool parsePrimary(std::string_view text, UaiskExpressionAst& outExpr) {
    text = trimView(text);
    if (text.empty()) {
        return false;
    }

    if (text.front() == '(') {
        if (text.back() != ')') {
            return false;
        }
        return parseOr(text.substr(1, text.size() - 2), outExpr);
    }

    return parseComparison(text, outExpr);
}

bool parseUnary(std::string_view text, UaiskExpressionAst& outExpr) {
    text = trimView(text);
    if (text.empty()) {
        return false;
    }

    if (text.front() == '!') {
        UaiskExpressionAst child;
        if (!parseUnary(text.substr(1), child)) {
            return false;
        }
        outExpr = UaiskExpressionAst{};
        outExpr.op = UaiskExpressionOp::Not;
        outExpr.valid = true;
        outExpr.children.push_back(std::move(child));
        return true;
    }

    return parsePrimary(text, outExpr);
}

bool splitAtOperator(std::string_view text, std::string_view opToken, std::vector<std::string_view>& outParts) {
    outParts.clear();
    std::size_t depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i + opToken.size() <= text.size(); ++i) {
        const char c = text[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth > 0) {
                --depth;
            }
        } else if (depth == 0 && text.substr(i, opToken.size()) == opToken) {
            outParts.push_back(trimView(text.substr(start, i - start)));
            start = i + opToken.size();
            i += opToken.size() - 1;
        }
    }
    outParts.push_back(trimView(text.substr(start)));
    return outParts.size() > 1;
}

bool parseAnd(std::string_view text, UaiskExpressionAst& outExpr) {
    text = trimView(text);
    std::vector<std::string_view> parts;
    if (splitAtOperator(text, "&&", parts)) {
        outExpr = UaiskExpressionAst{};
        outExpr.op = UaiskExpressionOp::And;
        outExpr.valid = true;
        for (std::string_view part : parts) {
            UaiskExpressionAst child;
            if (!parseUnary(part, child)) {
                return false;
            }
            outExpr.children.push_back(std::move(child));
        }
        return !outExpr.children.empty();
    }

    return parseUnary(text, outExpr);
}

bool parseOr(std::string_view text, UaiskExpressionAst& outExpr) {
    text = trimView(text);
    std::vector<std::string_view> parts;
    if (splitAtOperator(text, "||", parts)) {
        outExpr = UaiskExpressionAst{};
        outExpr.op = UaiskExpressionOp::Or;
        outExpr.valid = true;
        for (std::string_view part : parts) {
            UaiskExpressionAst child;
            if (!parseAnd(part, child)) {
                return false;
            }
            outExpr.children.push_back(std::move(child));
        }
        return !outExpr.children.empty();
    }

    return parseAnd(text, outExpr);
}

bool evaluateComparison(const UaiskExpressionAst& expr, const UaiskExpressionEvalContext& ctx) {
    const f32 value = fieldValue(ctx, expr.fieldName);
    switch (expr.op) {
    case UaiskExpressionOp::Less:
        return value < expr.threshold;
    case UaiskExpressionOp::Greater:
        return value > expr.threshold;
    case UaiskExpressionOp::Equal:
        return value == expr.threshold;
    case UaiskExpressionOp::Literal:
        return expr.threshold != 0.f;
    default:
        return false;
    }
}

} // namespace

bool parseExpressionAst(std::string_view text, UaiskExpressionAst& outExpr) {
    return parseOr(trimView(text), outExpr);
}

bool evaluateExpressionAst(const UaiskExpressionAst& expr, const UaiskExpressionEvalContext& ctx) {
    if (!expr.valid) {
        return false;
    }

    switch (expr.op) {
    case UaiskExpressionOp::And: {
        for (const UaiskExpressionAst& child : expr.children) {
            if (!evaluateExpressionAst(child, ctx)) {
                return false;
            }
        }
        return !expr.children.empty();
    }
    case UaiskExpressionOp::Or: {
        for (const UaiskExpressionAst& child : expr.children) {
            if (evaluateExpressionAst(child, ctx)) {
                return true;
            }
        }
        return false;
    }
    case UaiskExpressionOp::Not:
        return expr.children.empty() ? false : !evaluateExpressionAst(expr.children.front(), ctx);
    default:
        return evaluateComparison(expr, ctx);
    }
}

void collectExpressionLeaves(const UaiskExpressionAst& expr, std::vector<UaiskExpressionAst>& outLeaves) {
    if (!expr.valid) {
        return;
    }

    switch (expr.op) {
    case UaiskExpressionOp::And:
    case UaiskExpressionOp::Or:
    case UaiskExpressionOp::Not:
        for (const UaiskExpressionAst& child : expr.children) {
            collectExpressionLeaves(child, outLeaves);
        }
        break;
    default:
        outLeaves.push_back(expr);
        break;
    }
}

} // namespace fuse::ai::uaisk
