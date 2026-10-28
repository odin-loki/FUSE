#pragma once

namespace fuse::relight::logic {

enum class CondOp { True, False, Not, And, Or };

struct Cond {
    CondOp op = CondOp::False;
    int left = -1;
    int right = -1;
};

namespace detail {

inline bool eval_condition_at(const Cond* nodes, int count, int index, int depth) {
    if (nodes == nullptr || index < 0 || index >= count || depth > count) {
        return false;
    }
    const Cond& node = nodes[index];
    switch (node.op) {
    case CondOp::True:
        return true;
    case CondOp::False:
        return false;
    case CondOp::Not:
        return !eval_condition_at(nodes, count, node.left, depth + 1);
    case CondOp::And:
        return eval_condition_at(nodes, count, node.left, depth + 1) &&
               eval_condition_at(nodes, count, node.right, depth + 1);
    case CondOp::Or:
        return eval_condition_at(nodes, count, node.left, depth + 1) ||
               eval_condition_at(nodes, count, node.right, depth + 1);
    }
    return false;
}

} // namespace detail

[[nodiscard]] inline bool eval_condition(const Cond* nodes, int count, int index) {
    return detail::eval_condition_at(nodes, count, index, 0);
}

} // namespace fuse::relight::logic
