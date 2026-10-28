#pragma once

#include <fuse/types.hpp>

#include <cmath>
#include <utility>
#include <vector>

namespace fuse::gameplay {

class ThreatTable {
public:
    void add(u32 id, f32 amount) {
        if (id == 0 || !std::isfinite(amount) || amount <= 0.f) {
            return;
        }
        for (Entry& entry : entries_) {
            if (entry.id == id) {
                entry.threat += amount;
                return;
            }
        }
        entries_.push_back(Entry{id, amount});
    }

    void decay(f32 factor) {
        if (!std::isfinite(factor)) {
            factor = 0.f;
        }
        if (factor < 0.f) {
            factor = 0.f;
        } else if (factor > 1.f) {
            factor = 1.f;
        }
        std::vector<Entry> kept;
        kept.reserve(entries_.size());
        for (Entry& entry : entries_) {
            entry.threat *= factor;
            if (entry.threat > 0.01f) {
                kept.push_back(entry);
            }
        }
        entries_.swap(kept);
    }

    void clear(u32 id) {
        std::vector<Entry> kept;
        for (const Entry& entry : entries_) {
            if (entry.id != id) {
                kept.push_back(entry);
            }
        }
        entries_.swap(kept);
    }

    [[nodiscard]] u32 top() const {
        u32 bestId = 0;
        f32 best = 0.f;
        bool any = false;
        for (const Entry& entry : entries_) {
            if (!any || entry.threat > best || (entry.threat == best && entry.id < bestId)) {
                best = entry.threat;
                bestId = entry.id;
                any = true;
            }
        }
        return bestId;
    }

private:
    struct Entry {
        u32 id = 0;
        f32 threat = 0.f;
    };
    std::vector<Entry> entries_;
};

} // namespace fuse::gameplay
