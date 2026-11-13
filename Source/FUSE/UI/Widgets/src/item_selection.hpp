// QItemSelectionModel-style selection for the tree and table views: Qt selection modes (Single, Multi, Extended,
// Contiguous, None) and behaviours (Items, Rows, Columns). Single mode keeps the old semantics: the current item
// (value()) is the selection; the other modes keep an explicit key set.
#pragma once

#include "fuse/ui/widgets/widgets.hpp"

#include <set>
#include <vector>

namespace fuse::ui::widgets::detail {

struct ItemSelection {
    SelectionMode mode = SelectionMode::Single;
    SelectionBehavior behavior = SelectionBehavior::Rows;
    std::set<u64> keys;
    i64 anchorR = -1, anchorC = 0;

    static u64 cellKey(i64 r, i64 c) { return (u64(u32(r)) << 32) | u32(c); }
    u64 key(i64 r, i64 c) const {
        return behavior == SelectionBehavior::Rows ? u64(u32(r)) : behavior == SelectionBehavior::Columns ? u64(u32(c)) : cellKey(r, c);
    }
    bool explicitSet() const { return mode == SelectionMode::Multi || mode == SelectionMode::Extended || mode == SelectionMode::Contiguous; }
    bool has(i64 r, i64 c, i64 curR, i64 curC) const {
        if (mode == SelectionMode::None || r < 0) return false;
        if (mode == SelectionMode::Single) {
            if (behavior == SelectionBehavior::Rows) return r == curR;
            if (behavior == SelectionBehavior::Columns) return c == curC && curR >= 0;
            return r == curR && c == curC;
        }
        return keys.contains(key(r, c));
    }
    bool rowFullySelected(i64 r, u32 cols, i64 curR, i64 curC) const {
        if (behavior == SelectionBehavior::Rows) return has(r, 0, curR, curC);
        for (u32 c = 0; c < std::max(1u, cols); ++c) if (!has(r, c, curR, curC)) return false;
        return true;
    }
    /// keys for the rectangle anchor..(r, c); `rows(a, b, out)` lists the visual rows from a to b (inclusive, any order)
    template <class Rows>
    void range(i64 r, i64 c, Rows&& rows, std::set<u64>& out) const {
        const i64 ar = anchorR < 0 ? r : anchorR, ac = anchorR < 0 ? c : anchorC;
        if (behavior == SelectionBehavior::Columns) {
            for (i64 k = std::min(ac, c); k <= std::max(ac, c); ++k) out.insert(u64(u32(k)));
            return;
        }
        std::vector<i64> rr;
        rows(ar, r, rr);
        for (i64 row : rr) {
            if (behavior == SelectionBehavior::Rows) out.insert(u64(u32(row)));
            else for (i64 k = std::min(ac, c); k <= std::max(ac, c); ++k) out.insert(cellKey(row, k));
        }
    }
    /// Mouse press on (r, c) with modifiers (QAbstractItemView::selectionCommand). True when the set changed.
    template <class Rows>
    bool click(i64 r, i64 c, u8 mods, Rows&& rows) {
        if (!explicitSet()) return false;
        const auto before = keys;
        const bool shift = mods & kModShift, ctrl = mods & (kModCtrl | kModSuper);
        if (mode == SelectionMode::Multi) {
            const u64 k = key(r, c);
            if (!keys.erase(k)) keys.insert(k);
            anchorR = r; anchorC = c;
        } else if (shift) {
            std::set<u64> s = (mode == SelectionMode::Extended && ctrl) ? keys : std::set<u64>{};
            range(r, c, rows, s);
            keys = std::move(s);
        } else if (ctrl && mode == SelectionMode::Extended) {
            const u64 k = key(r, c);
            if (!keys.erase(k)) keys.insert(k);
            anchorR = r; anchorC = c;
        } else {
            keys = {key(r, c)};
            anchorR = r; anchorC = c;
        }
        return keys != before;
    }
    /// Keyboard move of the current item to (r, c).
    template <class Rows>
    bool navigate(i64 r, i64 c, u8 mods, Rows&& rows) {
        if (!explicitSet() || r < 0) return false;
        const auto before = keys;
        if (mode == SelectionMode::Multi) { // MultiSelection: arrows move the current item; Shift(+PageUp / Down...) adds the range
            if (!(mods & kModShift)) return false;
            std::set<u64> s = keys;
            range(r, c, rows, s);
            keys = std::move(s);
            return keys != before;
        }
        if (mods & kModShift) {
            std::set<u64> s;
            range(r, c, rows, s);
            keys = std::move(s);
        } else if ((mods & (kModCtrl | kModSuper)) && mode == SelectionMode::Extended) {
            return false; // Ctrl+arrow: move without selecting
        } else {
            keys = {key(r, c)};
            anchorR = r; anchorC = c;
        }
        return keys != before;
    }
    /// Space (Multi) / Ctrl+Space (Extended): toggle the current item.
    bool toggle(i64 r, i64 c) {
        if (!explicitSet() || r < 0) return false;
        const u64 k = key(r, c);
        if (!keys.erase(k)) keys.insert(k);
        anchorR = r; anchorC = c;
        return true;
    }
    bool setAll(std::set<u64> all) {
        if (mode != SelectionMode::Multi && mode != SelectionMode::Extended && mode != SelectionMode::Contiguous) return false;
        if (all == keys) return false;
        keys = std::move(all);
        return true;
    }
};

} // namespace fuse::ui::widgets::detail
