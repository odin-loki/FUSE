#pragma once

// WP-5.3 residency-aware LOD cut + streaming feedback, single-source (docs/compute-kernels.md), one
// cluster per item: kernel "geometry_stream_cut", the CPU reference of the GPU pass "stream.cut"
// (src/geometry_streaming/shaders/stream_cut.{comp,slang}, same expressions in the same order, `precise`).
//
// Cut. The WP-5.2 cut (dag_cut_kernel.hpp) draws c iff acc(self(c)) && !acc(parent(c)). With streaming,
// a group can only be refined (its members drawn instead of its produced clusters) when its page is
// resident, so acceptance becomes
//     acc'(G) = acc(G) || !resident(page(G))            ("a missing page is coarse enough")
//     in_cut(c) = acc'(producer(c)) && !acc'(group(c))
//               = (acc(self) || !resident(producer_page)) && !acc(parent) && resident(member_page)
// (leaves have no producer: acc'(.) = true). Every drawn cluster is therefore in a resident page.
// Watertight: the WP-5.2 argument needs acceptance to be monotone up the DAG (acc(G) => acc(K) for every
// group K that consumes clusters G produced). acc is (cook-time slack); !resident(page(G)) =>
// !resident(page(K)) is not true in general — but its contrapositive is exactly the residency closure
// the core_logic model enforces: page(G) depends on page(K) (cluster_page_file.hpp), and a Resident page
// has all its dependencies Resident. So resident(page(G)) => resident(page(K)), acc' is monotone, and the
// resident cut tiles the surface exactly once, for every residency state the model can reach. Missing
// detail falls back to the nearest resident ancestor: the clusters G produced (one level coarser), whose
// own pages are resident by the same closure.
//
// Feedback, per drawn cluster c (atomic max per page, so the result is order independent). Priorities
// share one scale: log2 of how far a group's projected error exceeds the threshold, i.e. the smallest
// k in [1, kPriorityLevels] with error * error_scale <= threshold * dist * 2^k (capped; terminal groups
// get the cap), plus kPriorityMin — powers of two keep it exact (no division, the products of
// lod_acceptable):
//   * keep    its member page with the priority of c's group (link.parent, not acceptable since c is
//             drawn): how much error dropping the page would bring back — the value the residency's
//             priority preemption compares against;
//   * refine  if !acc(self) (the view wants c's finer replacement, whose page is missing), the producer
//             page with the priority of c's producer group (link.self).
// A group's error >= its producers' and its sphere contains theirs, so a page's dependencies (coarser
// groups) score at least as high as the page: preemption never takes a page's own foundation.
// A page's first request (its atomic max saw 0) also appends the page to the request list; each drawn
// cluster is appended to the draw list (both lists: set semantics, order depends on scheduling).
//
// Feedback words (FeedbackLayout): [0] request-list count, [1] draw-list count, [2..3] 0,
//   [4, 4 + pages)                   per-page priority (0 = not requested)
//   [4 + pages, 4 + 2 pages)         request list (page ids)
// Draw list: a separate buffer of cluster ids (the page-aware mesh path hook).

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/dag/dag_cut_kernel.hpp>
#include <fuse/renderer/geometry_streaming/cluster_page_types.hpp>

#include <cmath>

#if !defined(__CUDA_ARCH__)
#include <atomic>
#endif

namespace fuse::renderer::geometry_streaming::stream_kernel {

using geometry::dag::DagClusterLink;
using geometry::dag::DagLodBounds;
using geometry::dag::kDagErrorTerminal;
using geometry::dag::cut_kernel::DagView;

inline constexpr const char* kName = "geometry_stream_cut";
inline constexpr u32 kWorkgroup = 64u;
inline constexpr u32 kPriorityMin = 1u;     ///< priorities are kPriorityMin + [1, kPriorityLevels] = 2..31
inline constexpr u32 kPriorityLevels = 30u;
inline constexpr u32 kFeedbackHeaderWords = 4u;
inline constexpr u32 kFeedbackRequestCount = 0u;
inline constexpr u32 kFeedbackDrawCount = 1u;

/// Word offsets of the feedback buffer for `pages` pages.
struct FeedbackLayout {
    u32 priorities = kFeedbackHeaderWords;
    u32 list = kFeedbackHeaderWords;
    u32 words = kFeedbackHeaderWords;
};
FUSE_HOST_DEVICE inline FeedbackLayout feedback_layout(u32 pages) {
    FeedbackLayout l{};
    l.priorities = kFeedbackHeaderWords;
    l.list = kFeedbackHeaderWords + pages;
    l.words = kFeedbackHeaderWords + 2u * pages;
    return l;
}

FUSE_HOST_DEVICE inline bool page_resident(const u32* bits, u32 page) {
    return page != kPageNone && ((bits[page >> 5u] >> (page & 31u)) & 1u) != 0u;
}

/// Priority of a non-acceptable LOD bound (see file comment); same expressions as
/// geometry::dag::cut_kernel::lod_acceptable up to the comparison. Terminal bounds: the cap.
FUSE_HOST_DEVICE inline u32 refine_priority(const DagLodBounds& b, const DagView& v) {
    if (!(b.error < kDagErrorTerminal)) {
        return kPriorityMin + kPriorityLevels;
    }
    const f32 dx = b.center[0] - v.camera[0];
    const f32 dy = b.center[1] - v.camera[1];
    const f32 dz = b.center[2] - v.camera[2];
    const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius;
    const f32 dist = d > v.znear ? d : v.znear;
    const f32 lhs = b.error * v.error_scale;
    f32 rhs = v.threshold * dist;
    u32 k = 0u;
    while (k < kPriorityLevels && lhs > rhs) {
        rhs = rhs * 2.f;
        ++k;
    }
    return kPriorityMin + (k > 0u ? k : 1u);
}

/// Decision for one cluster: in_cut, and the (page, priority) of its keep / refine feedback
/// (kPageNone when none).
struct ClusterDecision {
    u32 in_cut = 0u;
    u32 keep_page = kPageNone;
    u32 keep_priority = 0u;
    u32 refine_page = kPageNone;
    u32 refine_priority = 0u;
};

FUSE_HOST_DEVICE inline ClusterDecision decide(const DagClusterLink& link, const StreamClusterInfo& info, const u32* residentBits,
                                               const DagView& v) {
    ClusterDecision out{};
    const bool selfAcc = geometry::dag::cut_kernel::lod_acceptable(link.self, v);
    const bool producerOk = selfAcc || !page_resident(residentBits, info.producer_page);
    const bool parentAcc = geometry::dag::cut_kernel::lod_acceptable(link.parent, v);
    if (producerOk && !parentAcc && page_resident(residentBits, info.member_page)) {
        out.in_cut = 1u;
        out.keep_page = info.member_page;
        out.keep_priority = refine_priority(link.parent, v);
        if (!selfAcc && info.producer_page != kPageNone) {
            out.refine_page = info.producer_page;
            out.refine_priority = refine_priority(link.self, v);
        }
    }
    return out;
}

FUSE_HOST_DEVICE inline u32 atomic_max_u32(u32* address, u32 value) {
#if defined(__CUDA_ARCH__)
    return atomicMax(address, value);
#else
    std::atomic_ref<u32> a(*address);
    u32 old = a.load(std::memory_order_relaxed);
    while (old < value && !a.compare_exchange_weak(old, value, std::memory_order_relaxed)) {
    }
    return old;
#endif
}

struct Params {
    kernel::Span<const DagClusterLink> links;  ///< one per cluster id
    kernel::Span<const StreamClusterInfo> info; ///< one per cluster id
    kernel::Span<const u32> resident;          ///< page bitmask, (pages + 31) / 32 words
    DagView view{};
    u32 page_count = 0;
    kernel::Span<u32> cut;       ///< 0 / 1 per cluster id
    kernel::Span<u32> feedback;  ///< FeedbackLayout words, zeroed before the launch
    kernel::Span<u32> draw_list; ///< cluster ids, capacity = cluster count
};

/// Feedback for one page: atomic max of the priority; the first requester appends the page.
FUSE_HOST_DEVICE inline void submit(const Params& p, u32 page, u32 priority) {
    const FeedbackLayout l = feedback_layout(p.page_count);
    const u32 old = atomic_max_u32(&p.feedback[l.priorities + page], priority);
    if (old == 0u) {
        const u32 slot = kernel::global_atomic_add(&p.feedback[kFeedbackRequestCount], 1u);
        if (slot < p.page_count) {
            p.feedback[l.list + slot] = page;
        }
    }
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 c = idx.linear;
        const ClusterDecision d = decide(p.links[c], p.info[c], p.resident.data, p.view);
        p.cut[c] = d.in_cut;
        if (d.in_cut == 0u) {
            return;
        }
        const u32 slot = kernel::global_atomic_add(&p.feedback[kFeedbackDrawCount], 1u);
        if (slot < p.draw_list.size) {
            p.draw_list[slot] = c;
        }
        submit(p, d.keep_page, d.keep_priority);
        if (d.refine_page != kPageNone) {
            submit(p, d.refine_page, d.refine_priority);
        }
    }
};

inline kernel::KernelLaunch make_launch(u32 cluster_count) {
    return kernel::KernelLaunch{kName, kernel::extent1(cluster_count), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::geometry_streaming::stream_kernel
