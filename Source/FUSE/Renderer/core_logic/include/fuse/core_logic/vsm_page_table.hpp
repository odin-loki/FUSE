// Virtual shadow map page table + physical page pool (WP-0.8): pure logic, no Vulkan types.
//
// Virtual space: kLevels clipmap levels of kPagesPerAxis x kPagesPerAxis pages each
// (default VsmPageTable16k: 16k^2 texels per level with 128^2-texel pages = 128^2 pages,
// 16 levels). Physical space: kPhysPages pages (default 4096 = an 8k^2 atlas of 128^2 pages).
//
// Per frame:  begin_frame(frame) -> mark()/mark_rect() (page marking input, e.g. the GPU
//             request bitmask readback) -> update().
// update() touches requested+mapped pages (LRU -> most recent), then maps requested+unmapped
// pages from the free stack, evicting the least-recently-used page that was NOT requested this
// frame when the pool is empty. A requested page whose static-cache bit is clear lands in the
// render list (and is then considered rendered/cached). Pages that cannot be mapped are counted
// in `failed` (the renderer falls back to a coarser level).
// Static-page cache invalidation: invalidate_rect() (one level), invalidate_bounds() (a
// level-0 page-space box relative to the clipmap centre, projected onto every level), or
// invalidate_level(). Invalidation clears the cached bit only; mappings stay.
//
// Invariants (check_invariants(); property tests and the BMC harness assert them):
//   * a physical page is either on the free stack exactly once or owned by exactly one
//     virtual page, and that virtual page's entry points back at it (no double map, no
//     page mapped to two virtual pages);
//   * free-stack size + LRU size == kPhysPages (free-list conservation);
//   * the LRU list holds exactly the mapped pages, ordered by non-increasing lastUsed frame.
//
// Storage is fixed-size members (~4.1 bytes per virtual page + ~20 bytes per physical page);
// the default configuration is ~1.1 MiB, so allocate it once (static or at init), not on the
// stack. Every loop is bounded by kVirtualPages or kPhysPages.
#pragma once

#include "fuse/core_logic/cl_common.hpp"

namespace fuse {
namespace core_logic {

struct VsmUpdateStats {
    uint32_t requested;
    uint32_t alreadyMapped;
    uint32_t allocated;
    uint32_t evicted;
    uint32_t failed;
    uint32_t toRender;
};

// Inclusive box in level-0 page units relative to the clipmap centre (may be negative).
struct VsmBounds {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
};

template <uint32_t kPagesPerAxis, uint32_t kLevels, uint32_t kPhysPages>
class VsmPageTable {
public:
    static constexpr uint32_t kPagesPerLevel = kPagesPerAxis * kPagesPerAxis;
    // Spelled out from the template parameters (CBMC does not fold member constants that
    // refer to other member constants inside array bounds).
    static constexpr uint32_t kVirtualPages = kPagesPerAxis * kPagesPerAxis * kLevels;
    static constexpr uint32_t kWords = (kPagesPerAxis * kPagesPerAxis * kLevels + 63u) / 64u;
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    // Page-table entry: bit31 mapped, bit30 cached (content valid), bits 0..23 physical page.
    static constexpr uint32_t kPteMapped = 0x80000000u;
    static constexpr uint32_t kPteCached = 0x40000000u;
    static constexpr uint32_t kPtePhysMask = 0x00FFFFFFu;

    FUSE_CL_STATIC_ASSERT(kPagesPerAxis >= 1u && kLevels >= 1u && kLevels <= 30u, "bad virtual size");
    FUSE_CL_STATIC_ASSERT(kPhysPages >= 1u && kPhysPages <= kPtePhysMask, "bad physical size");

    void reset() {
        for (uint32_t v = 0u; v < kVirtualPages; ++v) {
            pte_[v] = 0u;
        }
        for (uint32_t w = 0u; w < kWords; ++w) {
            requested_[w] = 0u;
        }
        for (uint32_t p = 0u; p < kPhysPages; ++p) {
            owner_[p] = kNone;
            lastUsed_[p] = 0u;
            prev_[p] = kNone;
            next_[p] = kNone;
            freeStack_[p] = kPhysPages - 1u - p; // pops 0, 1, 2, ...
        }
        freeTop_ = kPhysPages;
        head_ = kNone;
        tail_ = kNone;
        lruCount_ = 0u;
        frame_ = 0u;
        renderCount_ = 0u;
    }

    // Frames must strictly increase. Clears the request marks and the render list. update() may
    // run several times per frame (marks accumulate; each update() rebuilds the render list).
    ClStatus begin_frame(uint32_t frame) {
        if (frame <= frame_) {
            return ClStatus::InvalidArgument;
        }
        frame_ = frame;
        for (uint32_t w = 0u; w < kWords; ++w) {
            requested_[w] = 0u;
        }
        renderCount_ = 0u;
        return ClStatus::Ok;
    }

    ClStatus mark(uint32_t level, uint32_t x, uint32_t y) {
        if (level >= kLevels || x >= kPagesPerAxis || y >= kPagesPerAxis) {
            return ClStatus::InvalidArgument;
        }
        const uint32_t v = index(level, x, y);
        requested_[v / 64u] |= 1ull << (v % 64u);
        return ClStatus::Ok;
    }

    // Inclusive rectangle on one level.
    ClStatus mark_rect(uint32_t level, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        if (level >= kLevels || x0 > x1 || y0 > y1 || x1 >= kPagesPerAxis || y1 >= kPagesPerAxis) {
            return ClStatus::InvalidArgument;
        }
        for (uint32_t y = y0; y <= y1; ++y) {
            for (uint32_t x = x0; x <= x1; ++x) {
                const uint32_t v = index(level, x, y);
                requested_[v / 64u] |= 1ull << (v % 64u);
            }
        }
        return ClStatus::Ok;
    }

    VsmUpdateStats update() {
        VsmUpdateStats st;
        st.requested = 0u;
        st.alreadyMapped = 0u;
        st.allocated = 0u;
        st.evicted = 0u;
        st.failed = 0u;
        st.toRender = 0u;
        renderCount_ = 0u; // the render list describes the latest update()
        // Pass 1: touch requested pages that are already mapped (so pass 2 never evicts them).
        for (uint32_t w = 0u; w < kWords; ++w) {
            if (requested_[w] == 0u) {
                continue;
            }
            const uint32_t base = w * 64u;
            const uint32_t lim = (kVirtualPages - base) < 64u ? (kVirtualPages - base) : 64u;
            for (uint32_t b = 0u; b < lim; ++b) {
                if (((requested_[w] >> b) & 1u) == 0u) {
                    continue;
                }
                const uint32_t v = base + b;
                ++st.requested;
                if ((pte_[v] & kPteMapped) != 0u) {
                    ++st.alreadyMapped;
                    const uint32_t p = pte_[v] & kPtePhysMask;
                    lru_unlink(p);
                    lru_push_front(p);
                    lastUsed_[p] = frame_;
                    if ((pte_[v] & kPteCached) == 0u) {
                        add_render(v);
                    }
                }
            }
        }
        // Pass 2: map requested pages that are not mapped yet.
        for (uint32_t w = 0u; w < kWords; ++w) {
            if (requested_[w] == 0u) {
                continue;
            }
            const uint32_t base = w * 64u;
            const uint32_t lim = (kVirtualPages - base) < 64u ? (kVirtualPages - base) : 64u;
            for (uint32_t b = 0u; b < lim; ++b) {
                const uint32_t v = base + b;
                if (((requested_[w] >> b) & 1u) == 0u || (pte_[v] & kPteMapped) != 0u) {
                    continue;
                }
                uint32_t p = kNone;
                if (freeTop_ > 0u) {
                    p = freeStack_[--freeTop_];
                } else if (lastUsed_[tail_] < frame_) {
                    // (free stack empty => all kPhysPages pages are on the LRU, so tail_ is valid)
                    p = tail_;
                    pte_[owner_[p]] = 0u; // evicted content is gone
                    lru_unlink(p);
                    ++st.evicted;
                } else {
                    ++st.failed;
                    continue;
                }
                owner_[p] = v;
                lastUsed_[p] = frame_;
                pte_[v] = kPteMapped | p;
                lru_push_front(p);
                ++st.allocated;
                add_render(v);
            }
        }
        st.toRender = renderCount_;
        return st;
    }

    // Returns the physical page or kNone.
    uint32_t lookup(uint32_t level, uint32_t x, uint32_t y) const {
        if (level >= kLevels || x >= kPagesPerAxis || y >= kPagesPerAxis) {
            return kNone;
        }
        const uint32_t e = pte_[index(level, x, y)];
        return (e & kPteMapped) != 0u ? (e & kPtePhysMask) : kNone;
    }

    bool is_cached(uint32_t level, uint32_t x, uint32_t y) const {
        if (level >= kLevels || x >= kPagesPerAxis || y >= kPagesPerAxis) {
            return false;
        }
        return (pte_[index(level, x, y)] & kPteCached) != 0u;
    }

    ClStatus unmap(uint32_t level, uint32_t x, uint32_t y) {
        if (level >= kLevels || x >= kPagesPerAxis || y >= kPagesPerAxis) {
            return ClStatus::InvalidArgument;
        }
        const uint32_t v = index(level, x, y);
        if ((pte_[v] & kPteMapped) == 0u) {
            return ClStatus::NotFound;
        }
        release(pte_[v] & kPtePhysMask);
        return ClStatus::Ok;
    }

    // Frees mapped pages whose last use is more than maxAge frames ago. Returns the count.
    uint32_t evict_older_than(uint32_t maxAge) {
        uint32_t n = 0u;
        // Each iteration frees one mapped page: at most kPhysPages iterations.
        while (tail_ != kNone && frame_ - lastUsed_[tail_] > maxAge) {
            release(tail_);
            ++n;
        }
        return n;
    }

    // Clears the cached bit of mapped pages in an inclusive rectangle (clamped). Returns count.
    uint32_t invalidate_rect(uint32_t level, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        if (level >= kLevels || x0 > x1 || y0 > y1 || x0 >= kPagesPerAxis || y0 >= kPagesPerAxis) {
            return 0u;
        }
        x1 = x1 < kPagesPerAxis ? x1 : kPagesPerAxis - 1u;
        y1 = y1 < kPagesPerAxis ? y1 : kPagesPerAxis - 1u;
        uint32_t n = 0u;
        for (uint32_t y = y0; y <= y1; ++y) {
            for (uint32_t x = x0; x <= x1; ++x) {
                uint32_t& e = pte_[index(level, x, y)];
                if ((e & kPteCached) != 0u) {
                    e &= ~kPteCached;
                    ++n;
                }
            }
        }
        return n;
    }

    // Projects a level-0 box (relative to the clipmap centre) onto every level: level L page
    // = floor(c / 2^L) + kPagesPerAxis/2, clamped to the level; boxes fully outside are skipped.
    uint32_t invalidate_bounds(const VsmBounds& b) {
        if (b.x0 > b.x1 || b.y0 > b.y1) {
            return 0u;
        }
        uint32_t n = 0u;
        for (uint32_t level = 0u; level < kLevels; ++level) {
            const int64_t half = static_cast<int64_t>(kPagesPerAxis / 2u);
            const int64_t lx0 = floor_shift(b.x0, level) + half;
            const int64_t ly0 = floor_shift(b.y0, level) + half;
            const int64_t lx1 = floor_shift(b.x1, level) + half;
            const int64_t ly1 = floor_shift(b.y1, level) + half;
            const int64_t maxc = static_cast<int64_t>(kPagesPerAxis) - 1;
            if (lx1 < 0 || ly1 < 0 || lx0 > maxc || ly0 > maxc) {
                continue;
            }
            n += invalidate_rect(level, static_cast<uint32_t>(lx0 < 0 ? 0 : lx0), static_cast<uint32_t>(ly0 < 0 ? 0 : ly0),
                                 static_cast<uint32_t>(lx1), static_cast<uint32_t>(ly1));
        }
        return n;
    }

    uint32_t invalidate_level(uint32_t level) {
        return invalidate_rect(level, 0u, 0u, kPagesPerAxis - 1u, kPagesPerAxis - 1u);
    }

    uint32_t free_count() const { return freeTop_; }
    uint32_t mapped_count() const { return lruCount_; }
    uint32_t frame() const { return frame_; }
    uint32_t render_count() const { return renderCount_; }
    // Virtual page indices (level * kPagesPerLevel + y * kPagesPerAxis + x) to render this frame.
    uint32_t render_page(uint32_t i) const { return i < renderCount_ ? renderList_[i] : kNone; }
    uint32_t owner_of(uint32_t phys) const { return phys < kPhysPages ? owner_[phys] : kNone; }
    uint32_t lru_tail() const { return tail_; }
    uint32_t last_used(uint32_t phys) const { return phys < kPhysPages ? lastUsed_[phys] : 0u; }

    // Non-static on purpose: CBMC 5.9x cannot resolve static member functions of class templates.
    uint32_t index(uint32_t level, uint32_t x, uint32_t y) const {
        return level * kPagesPerLevel + y * kPagesPerAxis + x;
    }

    bool check_invariants() const {
        uint8_t seen[kPhysPages];
        for (uint32_t p = 0u; p < kPhysPages; ++p) {
            seen[p] = 0u;
        }
        if (freeTop_ > kPhysPages) {
            return false;
        }
        for (uint32_t i = 0u; i < freeTop_; ++i) {
            const uint32_t p = freeStack_[i];
            if (p >= kPhysPages || seen[p] != 0u || owner_[p] != kNone) {
                return false;
            }
            seen[p] = 1u;
        }
        uint32_t count = 0u;
        uint32_t prev = kNone;
        uint32_t cur = head_;
        for (uint32_t i = 0u; i < kPhysPages && cur != kNone; ++i) {
            if (cur >= kPhysPages || seen[cur] != 0u || prev_[cur] != prev) {
                return false;
            }
            const uint32_t v = owner_[cur];
            if (v >= kVirtualPages || (pte_[v] & kPteMapped) == 0u || (pte_[v] & kPtePhysMask) != cur) {
                return false;
            }
            if (prev != kNone && lastUsed_[cur] > lastUsed_[prev]) {
                return false;
            }
            seen[cur] = 1u;
            ++count;
            prev = cur;
            cur = next_[cur];
        }
        if (cur != kNone || tail_ != prev || count != lruCount_ || freeTop_ + lruCount_ != kPhysPages) {
            return false;
        }
        // Every physical page is now known to be free (unowned) or on the LRU with a
        // consistent back-pointer; a mapped entry must therefore point at its own LRU page.
        for (uint32_t v = 0u; v < kVirtualPages; ++v) {
            if ((pte_[v] & kPteMapped) != 0u) {
                const uint32_t p = pte_[v] & kPtePhysMask;
                if (p >= kPhysPages || owner_[p] != v) {
                    return false; // dangling entry or two virtual pages on one physical page
                }
            } else if (pte_[v] != 0u) {
                return false; // cached bit without a mapping
            }
        }
        return true;
    }

private:
    friend struct ClTestAccess;

    int64_t floor_shift(int32_t v, uint32_t s) const {
        const int64_t d = static_cast<int64_t>(1) << s;
        const int64_t x = static_cast<int64_t>(v);
        return x >= 0 ? x / d : -((-x + d - 1) / d);
    }

    void add_render(uint32_t v) {
        // Every rendered page is mapped and appears once per frame, so renderCount_ <= kPhysPages.
        renderList_[renderCount_++] = v;
        pte_[v] |= kPteCached;
    }

    void release(uint32_t p) {
        pte_[owner_[p]] = 0u;
        owner_[p] = kNone;
        lru_unlink(p);
        freeStack_[freeTop_++] = p;
    }

    void lru_unlink(uint32_t p) {
        const uint32_t pr = prev_[p];
        const uint32_t nx = next_[p];
        if (pr != kNone) {
            next_[pr] = nx;
        } else {
            head_ = nx;
        }
        if (nx != kNone) {
            prev_[nx] = pr;
        } else {
            tail_ = pr;
        }
        prev_[p] = kNone;
        next_[p] = kNone;
        --lruCount_;
    }

    void lru_push_front(uint32_t p) {
        prev_[p] = kNone;
        next_[p] = head_;
        if (head_ != kNone) {
            prev_[head_] = p;
        } else {
            tail_ = p;
        }
        head_ = p;
        ++lruCount_;
    }

    uint32_t pte_[kPagesPerAxis * kPagesPerAxis * kLevels];
    uint64_t requested_[(kPagesPerAxis * kPagesPerAxis * kLevels + 63u) / 64u];
    uint32_t owner_[kPhysPages];
    uint32_t lastUsed_[kPhysPages];
    uint32_t prev_[kPhysPages];
    uint32_t next_[kPhysPages];
    uint32_t freeStack_[kPhysPages];
    uint32_t renderList_[kPhysPages];
    uint32_t freeTop_;
    uint32_t head_;
    uint32_t tail_;
    uint32_t lruCount_;
    uint32_t frame_;
    uint32_t renderCount_;
};

// 16k^2 virtual texels per clipmap level, 128^2-texel pages, 16 levels, 4096 physical pages.
typedef VsmPageTable<128u, 16u, 4096u> VsmPageTable16k;

} // namespace core_logic
} // namespace fuse
