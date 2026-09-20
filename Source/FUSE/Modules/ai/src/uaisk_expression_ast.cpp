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

float parseLiteral(std::string_view text) {
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

} // namespace

bool parseExpressionAst(std::string_view text, UaiskExpressionAst& outExpr) {
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

} // namespace fuse::ai::uaisk
