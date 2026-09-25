// WP-3.1 virtual shadow map page logic: clipmap windows, page table, physical page pool with
// age-bucketed LRU, static-page caching with invalidation. Pure logic, no Vulkan types, same
// CBMC-parsable dialect as the WP-0.8 cores (cl_common.hpp).
//
// This is the exact CPU model of the GPU passes in src/shadow/vsm/** (src/shadow/vsm/shaders/*):
// fed the same request and invalidation bitmasks, update() leaves the same page table, the same
// physical-page metadata and the same statistics as the GPU frame, bit for bit (the render list
// is the same set; the GPU appends it with atomics, so only its order differs).
//
// Virtual space: `levels` clipmap levels (<= kLevels) of kAxis x kAxis pages (kAxis a power of
// two; 128 x 128-texel pages = 16k^2 texels per level). Each level is a window of kAxis pages
// centred on an absolute page coordinate `origin` (the camera's page in that level's grid):
// absolute pages [origin - kAxis/2, origin + kAxis/2) are resident in the window. The table is
// addressed toroidally: absolute page a lives in slot a mod kAxis, so moving the window only
// changes the meaning of the slots that scrolled (their cached content is dropped; the physical
// page stays mapped and is re-rendered for the new absolute page).
//
// Page-table entry (PTE, u32): bit 31 mapped, bit 30 cached (content valid), bits 0..23 the
// physical page. Physical page p: owner (virtual page or kClInvalid) and lastUsed (frame).
//
// One frame, in GPU pass order (update() runs all of it; the pieces are public for tests):
//   vsm.invalidate  every virtual page set in `invalidWords` loses its cached bit
//                   (GPU-scene bounds changes, see vsm_kernel.hpp);
//   vsm.update      per virtual page: the cached bit is dropped when its level is invalid this
//                   frame (depth key changed, invalidate-all) or its slot now holds another
//                   absolute page (window scrolled); requested + mapped pages are touched
//                   (lastUsed = frame); requested + unmapped pages go to the need set;
//   vsm.alloc       needs in ascending virtual-page order get physical pages in candidate order:
//                   candidates are the pages not used this frame, free pages first, then owned
//                   pages by age bucket min(frame - lastUsed, kVsmAgeBuckets - 1) descending,
//                   ties by ascending physical index (LRU exact up to 14 frames of age); an owned
//                   candidate is evicted (its old virtual page unmapped). Needs beyond the
//                   candidates fail (stay unmapped; the renderer falls back to a coarser level);
//   vsm.render      requested + mapped + not cached pages form the render list and become cached.
//
// Invariants (check_invariants(); property tests and the BMC harness assert them):
//   * every owned physical page is mapped by exactly its owner, every mapped PTE points at a
//     physical page it owns (no double map, no page on two virtual pages);
//   * free pages (owner == kClInvalid) + owned pages == physPages (conservation);
//   * no cached bit without a mapping; no PTE bits outside mapped | cached | phys;
//   * lastUsed <= frame for owned pages.
// Storage: fixed-size members (~4.25 bytes per virtual page + 16 bytes per physical page); the
// 16-level 16k configuration is ~1.2 MiB, so allocate it once (static or at init). No heap.
#pragma once

#include "fuse/core_logic/cl_common.hpp"

namespace fuse {
namespace core_logic {

static constexpr uint32_t kVsmPteMapped = 0x80000000u;
static constexpr uint32_t kVsmPteCached = 0x40000000u;
static constexpr uint32_t kVsmPtePhysMask = 0x00FFFFFFu;
/// Candidate keys: 1..kVsmAgeBuckets-1 = age bucket of an owned page, kVsmFreeKey = free page.
static constexpr uint32_t kVsmAgeBuckets = 16u;
static constexpr uint32_t kVsmFreeKey = 16u;
static constexpr uint32_t kVsmMaxLevels = 16u;
/// Window origins must stay within +-kVsmMaxOrigin pages (all slot arithmetic fits in int32).
static constexpr int32_t kVsmMaxOrigin = 0x20000000;

/// Absolute page coordinate held by wrapped slot `slot` of a window of `axis` pages (power of
/// two) centred on `origin`: the unique a == slot (mod axis) in [origin - axis/2, origin + axis/2).
inline int32_t vsm_slot_to_abs(uint32_t slot, int32_t origin, uint32_t axis) {
    const int64_t base = static_cast<int64_t>(origin) - static_cast<int64_t>(axis / 2u);
    const uint32_t off = static_cast<uint32_t>(static_cast<int64_t>(slot) - base) & (axis - 1u);
    return static_cast<int32_t>(base + static_cast<int64_t>(off));
}

/// Wrapped slot of an absolute page coordinate.
inline uint32_t vsm_abs_to_slot(int32_t a, uint32_t axis) { return static_cast<uint32_t>(a) & (axis - 1u); }

/// Whether absolute page `a` lies in the window centred on `origin`.
inline bool vsm_in_window(int32_t a, int32_t origin, uint32_t axis) {
    const int64_t d = static_cast<int64_t>(a) - (static_cast<int64_t>(origin) - static_cast<int64_t>(axis / 2u));
    return d >= 0 && d < static_cast<int64_t>(axis);
}

/// Candidate key of a physical page (0 = used this frame, not a candidate).
inline uint32_t vsm_candidate_key(uint32_t owner, uint32_t lastUsed, uint32_t frame) {
    if (owner == kClInvalid) {
        return kVsmFreeKey;
    }
    const uint32_t age = frame - lastUsed;
    return age < kVsmAgeBuckets - 1u ? age : kVsmAgeBuckets - 1u;
}

/// Placement of one clipmap level for a frame.
struct VsmLevelPlacement {
    int32_t originX;  ///< window centre page (absolute, level grid)
    int32_t originY;
    int32_t depthKey; ///< snapped light-space depth of the level's range; a change invalidates the level
    int32_t pad;
};

struct VsmFrameInput {
    uint32_t frame;         ///< strictly increasing, >= 1
    uint32_t levels;        ///< must equal the pool's level count
    uint32_t invalidateAll; ///< non-zero: drop every cached bit (light rotation changed, ...)
    uint32_t pad;
    VsmLevelPlacement level[kVsmMaxLevels];
};

/// Statistics of one update(); the GPU frame counts the same values.
struct VsmPageStats {
    uint32_t requested;     ///< request bits set
    uint32_t alreadyMapped; ///< requested pages mapped before the allocation
    uint32_t needed;        ///< requested pages unmapped before the allocation
    uint32_t candidates;    ///< physical pages not used this frame (free or evictable)
    uint32_t allocated;     ///< needs that received a physical page
    uint32_t evicted;       ///< of those, pages taken from another virtual page
    uint32_t failed;        ///< needs left unmapped (pool exhausted by this frame's requests)
    uint32_t toRender;      ///< render-list length
    uint32_t invalidated;   ///< cached bits dropped by the invalidation bitmask
    uint32_t scrolled;      ///< cached bits dropped by window movement / level invalidation
};

inline void vsm_clear_stats(VsmPageStats& s) {
    s.requested = 0u;
    s.alreadyMapped = 0u;
    s.needed = 0u;
    s.candidates = 0u;
    s.allocated = 0u;
    s.evicted = 0u;
    s.failed = 0u;
    s.toRender = 0u;
    s.invalidated = 0u;
    s.scrolled = 0u;
}

template <uint32_t kAxis, uint32_t kLevels, uint32_t kPhys>
class VsmPagePool {
public:
    static constexpr uint32_t kPagesPerLevel = kAxis * kAxis;
    static constexpr uint32_t kVirtualPages = kAxis * kAxis * kLevels;
    static constexpr uint32_t kWords = (kAxis * kAxis * kLevels + 31u) / 32u;

    FUSE_CL_STATIC_ASSERT(kAxis >= 2u && (kAxis & (kAxis - 1u)) == 0u, "axis must be a power of two >= 2");
    FUSE_CL_STATIC_ASSERT(kLevels >= 1u && kLevels <= kVsmMaxLevels, "bad level count");
    FUSE_CL_STATIC_ASSERT(kPhys >= 1u && kPhys <= kVsmPtePhysMask, "bad physical size");

    /// Empty pool: `levels` <= kLevels active levels, `physPages` <= kPhys physical pages.
    ClStatus reset(uint32_t levels, uint32_t physPages) {
        if (levels == 0u || levels > kLevels || physPages == 0u || physPages > kPhys) {
            levels_ = 0u;
            phys_ = 0u;
            return ClStatus::InvalidArgument;
        }
        levels_ = levels;
        phys_ = physPages;
        for (uint32_t v = 0u; v < kVirtualPages; ++v) {
            pte_[v] = 0u;
        }
        for (uint32_t w = 0u; w < kWords; ++w) {
            need_[w] = 0u;
        }
        for (uint32_t p = 0u; p < kPhys; ++p) {
            owner_[p] = kClInvalid;
            lastUsed_[p] = 0u;
            cand_[p] = kClInvalid;
            renderPage_[p] = kClInvalid;
            renderPhys_[p] = kClInvalid;
        }
        for (uint32_t l = 0u; l < kVsmMaxLevels; ++l) {
            placement_[l].originX = 0;
            placement_[l].originY = 0;
            placement_[l].depthKey = 0;
            placement_[l].pad = 0;
        }
        placed_ = 0u;
        frame_ = 0u;
        renderCount_ = 0u;
        candCount_ = 0u;
        return ClStatus::Ok;
    }

    /// Whole frame (see the header comment). `requestWords` / `invalidWords` hold one bit per
    /// virtual page (bit v % 32 of word v / 32); `invalidWords` may be null. Fails without
    /// touching the state for a non-increasing frame, a level-count mismatch or an origin out of
    /// range.
    ClStatus update(const VsmFrameInput& in, const uint32_t* requestWords, const uint32_t* invalidWords, VsmPageStats& st) {
        vsm_clear_stats(st);
        if (levels_ == 0u || requestWords == 0 || in.frame <= frame_ || in.levels != levels_) {
            return ClStatus::InvalidArgument;
        }
        for (uint32_t l = 0u; l < levels_; ++l) {
            if (!origin_ok(in.level[l].originX) || !origin_ok(in.level[l].originY)) {
                return ClStatus::InvalidArgument;
            }
        }
        frame_ = in.frame;
        if (invalidWords != 0) {
            apply_invalidation(invalidWords, st);
        }
        update_pages(in, requestWords, st);
        allocate(st);
        build_render_list(requestWords, st);
        return ClStatus::Ok;
    }

    // --- inspection ---------------------------------------------------------------------------
    uint32_t levels() const { return levels_; }
    uint32_t phys_pages() const { return phys_; }
    uint32_t frame() const { return frame_; }
    uint32_t active_pages() const { return levels_ * kPagesPerLevel; }
    uint32_t pte(uint32_t v) const { return v < kVirtualPages ? pte_[v] : 0u; }
    uint32_t owner(uint32_t p) const { return p < kPhys ? owner_[p] : kClInvalid; }
    uint32_t last_used(uint32_t p) const { return p < kPhys ? lastUsed_[p] : 0u; }
    uint32_t render_count() const { return renderCount_; }
    uint32_t render_page(uint32_t i) const { return i < renderCount_ && i < kPhys ? renderPage_[i] : kClInvalid; }
    uint32_t render_phys(uint32_t i) const { return i < renderCount_ && i < kPhys ? renderPhys_[i] : kClInvalid; }
    /// Candidate order of the last allocation (for the GPU parity gate).
    uint32_t candidate_count() const { return candCount_; }
    uint32_t candidate(uint32_t i) const { return i < candCount_ && i < kPhys ? cand_[i] : kClInvalid; }
    bool placed() const { return placed_ != 0u; }
    const VsmLevelPlacement& placement(uint32_t level) const { return placement_[level < kVsmMaxLevels ? level : 0u]; }
    uint32_t mapped_count() const {
        uint32_t n = 0u;
        for (uint32_t p = 0u; p < phys_; ++p) {
            n += owner_[p] != kClInvalid ? 1u : 0u;
        }
        return n;
    }
    uint32_t free_count() const { return phys_ - mapped_count(); }

    uint32_t index(uint32_t level, uint32_t sx, uint32_t sy) const { return level * kPagesPerLevel + sy * kAxis + sx; }
    /// Physical page of the page holding absolute page (ax, ay) of `level`, or kClInvalid.
    uint32_t lookup_abs(uint32_t level, int32_t ax, int32_t ay) const {
        if (level >= levels_ || level >= kLevels || placed_ == 0u || !vsm_in_window(ax, placement_[level].originX, kAxis) ||
            !vsm_in_window(ay, placement_[level].originY, kAxis)) {
            return kClInvalid;
        }
        const uint32_t e = pte_[index(level, vsm_abs_to_slot(ax, kAxis), vsm_abs_to_slot(ay, kAxis))];
        return (e & kVsmPteMapped) != 0u ? (e & kVsmPtePhysMask) : kClInvalid;
    }

    bool check_invariants() const {
        if (phys_ > kPhys || levels_ > kLevels) {
            return false;
        }
        for (uint32_t p = 0u; p < kPhys; ++p) {
            const uint32_t v = owner_[p];
            if (v == kClInvalid) {
                continue;
            }
            if (p >= phys_ || v >= kVirtualPages || v >= levels_ * kPagesPerLevel || (pte_[v] & kVsmPteMapped) == 0u ||
                (pte_[v] & kVsmPtePhysMask) != p || lastUsed_[p] > frame_) {
                return false;
            }
        }
        for (uint32_t v = 0u; v < kVirtualPages; ++v) {
            const uint32_t e = pte_[v];
            if ((e & ~(kVsmPteMapped | kVsmPteCached | kVsmPtePhysMask)) != 0u) {
                return false;
            }
            if ((e & kVsmPteMapped) != 0u) {
                const uint32_t p = e & kVsmPtePhysMask;
                if (p >= phys_ || p >= kPhys || owner_[p] != v) {
                    return false; // dangling entry or two virtual pages on one physical page
                }
            } else if (e != 0u) {
                return false; // cached bit (or stale physical index) without a mapping
            }
        }
        return true;
    }

private:
    friend struct ClTestAccess;

    // Non-static on purpose: CBMC 5.9x cannot resolve static member functions of class templates.
    bool origin_ok(int32_t o) const { return o >= -kVsmMaxOrigin && o <= kVsmMaxOrigin; }

    bool bit(const uint32_t* words, uint32_t v) const { return ((words[v / 32u] >> (v % 32u)) & 1u) != 0u; }

    void apply_invalidation(const uint32_t* invalidWords, VsmPageStats& st) {
        const uint32_t active = levels_ * kPagesPerLevel;
        for (uint32_t v = 0u; v < active; ++v) {
            if (bit(invalidWords, v) && (pte_[v] & kVsmPteCached) != 0u) {
                pte_[v] &= ~kVsmPteCached;
                ++st.invalidated;
            }
        }
    }

    void update_pages(const VsmFrameInput& in, const uint32_t* requestWords, VsmPageStats& st) {
        const uint32_t active = levels_ * kPagesPerLevel;
        for (uint32_t w = 0u; w < kWords; ++w) {
            need_[w] = 0u;
        }
        for (uint32_t v = 0u; v < active; ++v) {
            const uint32_t level = v / kPagesPerLevel;
            const uint32_t r = v % kPagesPerLevel;
            const int32_t curX = in.level[level].originX;
            const int32_t curY = in.level[level].originY;
            const int32_t curKey = in.level[level].depthKey;
            // First frame: nothing is cached, the previous placement is the current one. (Scalars, not a
            // conditional reference: CBMC 5.9x symex rejects the latter.)
            int32_t prevX = curX;
            int32_t prevY = curY;
            int32_t prevKey = curKey;
            if (placed_ != 0u) {
                prevX = placement_[level].originX;
                prevY = placement_[level].originY;
                prevKey = placement_[level].depthKey;
            }
            bool changed = in.invalidateAll != 0u || prevKey != curKey;
            if (!changed) {
                const uint32_t sx = r % kAxis;
                const uint32_t sy = r / kAxis;
                changed = vsm_slot_to_abs(sx, prevX, kAxis) != vsm_slot_to_abs(sx, curX, kAxis) ||
                          vsm_slot_to_abs(sy, prevY, kAxis) != vsm_slot_to_abs(sy, curY, kAxis);
            }
            if (changed && (pte_[v] & kVsmPteCached) != 0u) {
                pte_[v] &= ~kVsmPteCached;
                ++st.scrolled;
            }
            if (!bit(requestWords, v)) {
                continue;
            }
            ++st.requested;
            if ((pte_[v] & kVsmPteMapped) != 0u) {
                ++st.alreadyMapped;
                lastUsed_[pte_[v] & kVsmPtePhysMask] = frame_;
            } else {
                need_[v / 32u] |= 1u << (v % 32u);
                ++st.needed;
            }
        }
        for (uint32_t l = 0u; l < levels_; ++l) {
            placement_[l] = in.level[l];
        }
        placed_ = 1u;
    }

    void allocate(VsmPageStats& st) {
        candCount_ = 0u;
        for (uint32_t key = kVsmFreeKey; key >= 1u; --key) {
            for (uint32_t p = 0u; p < phys_; ++p) {
                if (vsm_candidate_key(owner_[p], lastUsed_[p], frame_) == key) {
                    cand_[candCount_++] = p;
                }
            }
        }
        st.candidates = candCount_;
        const uint32_t active = levels_ * kPagesPerLevel;
        uint32_t i = 0u;
        for (uint32_t v = 0u; v < active; ++v) {
            if (!bit(need_, v)) {
                continue;
            }
            if (i >= candCount_) {
                ++st.failed;
                continue;
            }
            const uint32_t p = cand_[i++];
            if (owner_[p] != kClInvalid) {
                pte_[owner_[p]] = 0u; // the evicted content is gone
                ++st.evicted;
            }
            owner_[p] = v;
            lastUsed_[p] = frame_;
            pte_[v] = kVsmPteMapped | p;
            ++st.allocated;
        }
    }

    void build_render_list(const uint32_t* requestWords, VsmPageStats& st) {
        renderCount_ = 0u;
        const uint32_t active = levels_ * kPagesPerLevel;
        for (uint32_t v = 0u; v < active; ++v) {
            const uint32_t e = pte_[v];
            if (bit(requestWords, v) && (e & kVsmPteMapped) != 0u && (e & kVsmPteCached) == 0u) {
                // Every mapped page appears once, so renderCount_ <= phys_.
                renderPage_[renderCount_] = v;
                renderPhys_[renderCount_] = e & kVsmPtePhysMask;
                ++renderCount_;
                pte_[v] = e | kVsmPteCached;
            }
        }
        st.toRender = renderCount_;
    }

    uint32_t pte_[kAxis * kAxis * kLevels];
    uint32_t need_[(kAxis * kAxis * kLevels + 31u) / 32u];
    uint32_t owner_[kPhys];
    uint32_t lastUsed_[kPhys];
    uint32_t cand_[kPhys];
    uint32_t renderPage_[kPhys];
    uint32_t renderPhys_[kPhys];
    VsmLevelPlacement placement_[kVsmMaxLevels];
    uint32_t levels_;
    uint32_t phys_;
    uint32_t placed_;
    uint32_t frame_;
    uint32_t renderCount_;
    uint32_t candCount_;
};

/// 128 x 128 pages per level (16k^2 texels with 128^2-texel pages), 16 levels, up to 4096
/// physical pages (an 8k^2 pool).
typedef VsmPagePool<128u, 16u, 4096u> VsmPagePool16k;

} // namespace core_logic
} // namespace fuse
