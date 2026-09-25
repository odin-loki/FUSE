// WP-5.3 cluster streaming and residency, CPU gates (no device; also run in the stub tree). Lavapipe
// gates: test_rp_geometry_streaming.cpp. Core residency model: core_logic/residency (its own gates).
//
//   layout      page files of 3 meshes x 3 page sizes: validate_cluster_page_file (every cluster once, in
//               its group's page, with the cooked record / triangles / VPOS), coarse pages == the terminal
//               groups exactly, dependencies topological and == the definition, payloads <= page_bytes; a
//               group larger than page_bytes fails the build
//   format      FCPG round trip bit for bit, deterministic bytes, every truncation / 300 bit flips / bad
//               magic / version / reserved / structural corruptions (checksum recomputed) rejected, file I/O
//   kernel      "geometry_stream_cut": all pages resident == geometry_dag_cut (WP-5.2) over 400 views;
//               CpuReference == CpuParallel (cut, priorities, lists as sets); under 600 random
//               dependency-closed residency states x views: every cut watertight and area-bounded from page
//               bytes alone, drawn clusters only in resident pages, feedback == its definition (keep
//               priorities for drawn clusters' pages, refine priorities for missing producer pages), priorities
//               2..31 non-decreasing as the projected error grows; mutation: residency
//               states that break the closure produce cracks (the closure is what the gate relies on)
//   priority    cut-driven ordering: with one load per frame, the page of the most-exceeded error loads
//               first; ancestors (dependencies) load before their dependents
//   flythrough  scripted fly-through (bumpy icosphere, 82k triangles, 300 frames, IO latency 2 frames, 3%
//               failed loads, feedback latency 1 frame) with a generous, a tight and a shrinking budget:
//               0 frames missing geometry (every frame's cut is watertight and area-bounded using only the
//               bytes of resident pool slots; evicted slots are poisoned), coarse LOD resident on every
//               frame, budget never exceeded, and after the camera holds still the resident cut converges
//               to the ideal cut (generous budget)
//   zero_alloc  steady-state frames of the fly-through loop: 0 operator-new calls in ClusterStreamer
//               (begin_frame / apply_feedback / update / complete_load / fail_load) and the kernel launch
#include "test_rp_geometry_streaming_common.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/core_logic/cluster_page_residency.hpp>
#include <fuse/renderer/geometry_streaming/cluster_streamer.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <random>
#include <set>

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

// GCC may inline the replacement operators into callers and then report a false
// -Wmismatched-new-delete at -O2; replacement functions must not be inline anyway.
#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace stream_test;
namespace sk = gs::stream_kernel;
using fuse::kernel::Backend;

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

// --- assets -------------------------------------------------------------------------------------------
struct Assets {
    StreamAsset big;    ///< bumpy icosphere, 6 subdivisions (81,920 triangles), radius 10
    StreamAsset medium; ///< bumpy icosphere, 5 subdivisions
    StreamAsset small;  ///< smooth icosphere, 3 subdivisions (few pages)
    bool ok = false;
};

Assets& assets() {
    static Assets a;
    static bool built = false;
    if (!built) {
        built = true;
        std::string err;
        a.ok = build_asset(make_bumpy_sphere(6, 10.f, 0.08f), 32u * 1024u, a.big, &err) &&
               build_asset(make_bumpy_sphere(5, 3.f, 0.12f), 32u * 1024u, a.medium, &err) &&
               build_asset(make_bumpy_sphere(3, 1.f, 0.f), 32u * 1024u, a.small, &err);
        if (!a.ok) {
            std::fprintf(stderr, "asset build failed: %s\n", err.c_str());
        } else {
            std::printf("assets: big %u clusters / %zu groups / %u pages (%u coarse, %zu dependency edges); medium %u pages; "
                        "small %u pages\n",
                        a.big.mesh.dag.cluster_count(), a.big.mesh.dag.groups.size(), a.big.pages.page_count(),
                        a.big.pages.coarse_page_count, a.big.pages.page_deps.size(), a.medium.pages.page_count(),
                        a.small.pages.page_count());
        }
    }
    return a;
}

/// Leaf-cut area (the source surface as cooked).
f64 leafArea(const StreamAsset& a) {
    std::vector<u32> cut(a.mesh.dag.cluster_count(), 0u);
    for (u32 c = 0; c < a.mesh.dag.leaf_cluster_count; ++c) {
        cut[c] = 1u;
    }
    return check_cut(a, cut, [&](u32 p) { return a.pages.page_payload(p); }).area;
}

/// Runs the stream kernel over the asset.
struct KernelRun {
    std::vector<u32> cut, feedback, draw;
};
void runKernel(const StreamAsset& a, const std::vector<u32>& bits, const dag::cut_kernel::DagView& view, KernelRun& out,
               Backend backend = Backend::CpuParallel) {
    const u32 n = a.mesh.dag.cluster_count();
    const u32 pages = a.pages.page_count();
    out.cut.assign(n, 0u);
    out.feedback.assign(sk::feedback_layout(pages).words, 0u);
    out.draw.assign(n, 0u);
    sk::Params p{};
    p.links = fuse::kernel::make_span(a.mesh.dag.links.data(), n);
    p.info = fuse::kernel::make_span(a.info.data(), n);
    p.resident = fuse::kernel::make_span(bits.data(), static_cast<u32>(bits.size()));
    p.view = view;
    p.page_count = pages;
    p.cut = fuse::kernel::make_span(out.cut.data(), n);
    p.feedback = fuse::kernel::make_span(out.feedback.data(), static_cast<u32>(out.feedback.size()));
    p.draw_list = fuse::kernel::make_span(out.draw.data(), n);
    (void)fuse::kernel::launch(backend, sk::make_launch(n), sk::Kernel{}, p);
}

std::vector<u32> allResident(u32 pages) {
    std::vector<u32> bits((pages + 31u) / 32u, 0u);
    for (u32 p = 0; p < pages; ++p) {
        bits[p >> 5u] |= 1u << (p & 31u);
    }
    return bits;
}

dag::cut_kernel::DagView randomView(std::mt19937& rng, f32 radius) {
    std::uniform_real_distribution<f32> dir(-1.f, 1.f);
    std::uniform_real_distribution<f32> dist(1.02f, 8.f);
    std::uniform_real_distribution<f32> thr(0.25f, 8.f);
    f32 d[3] = {dir(rng), dir(rng), dir(rng)};
    const f32 len = std::max(1e-3f, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    const f32 r = radius * dist(rng);
    const f32 cam[3] = {d[0] / len * r, d[1] / len * r, d[2] / len * r};
    return make_view(cam, thr(rng));
}

// --- layout ------------------------------------------------------------------------------------------
void testLayout() {
    Assets& A = assets();
    expect(A.ok, "assets build");
    if (!A.ok) {
        return;
    }
    for (const StreamAsset* a : {&A.big, &A.medium, &A.small}) {
        for (u32 bytes : {32u * 1024u, 64u * 1024u, 128u * 1024u}) {
            gs::ClusterPageFile f;
            std::string err;
            gs::PageBuildOptions o{};
            o.page_bytes = bytes;
            const bool built = gs::build_cluster_pages(a->mesh, o, f, &err);
            expect(built, "build_cluster_pages: " + err);
            if (!built) {
                continue;
            }
            expect(gs::validate_cluster_page_file(f, a->mesh, &err), "validate_cluster_page_file: " + err);
            u32 coarseGroups = 0, terminal = 0;
            u64 used = 0;
            for (u32 p = 0; p < f.page_count(); ++p) {
                const gs::ClusterPageEntry& e = f.pages[p];
                used += e.payload_bytes;
                expect(e.payload_bytes <= bytes, "payload within page_bytes");
                if (p < f.coarse_page_count) {
                    coarseGroups += e.group_count;
                    expect(e.dep_count == 0u, "coarse pages have no dependencies");
                }
                for (u32 i = 0; i < e.dep_count; ++i) {
                    expect(f.page_deps[e.dep_offset + i] < p, "dependencies point at earlier pages");
                }
            }
            for (const dag::DagGroup& g : a->mesh.dag.groups) {
                terminal += g.child_count == 0u ? 1u : 0u;
            }
            expect(coarseGroups == terminal, "coarse pages hold exactly the terminal groups");
            expect(f.page_count() > (a == &A.small ? 0u : 2u), "several pages");
            std::printf("layout: %u clusters, %u groups (%u terminal), page %u KiB -> %u pages (%u coarse), %zu deps, fill %.0f%%\n",
                        f.cluster_count, f.group_count, terminal, bytes / 1024u, f.page_count(), f.coarse_page_count,
                        f.page_deps.size(), 100.0 * static_cast<f64>(used) / (static_cast<f64>(f.page_count()) * bytes));
        }
    }
    gs::ClusterPageFile f;
    std::string err;
    gs::PageBuildOptions tiny{};
    tiny.page_bytes = 1024u;
    expect(!gs::build_cluster_pages(A.big.mesh, tiny, f, &err) && err.find("page_bytes") != std::string::npos,
           "a group larger than page_bytes fails the build");
    tiny.page_bytes = 1000u;
    expect(!gs::build_cluster_pages(A.big.mesh, tiny, f, &err), "page_bytes must be a multiple of 16");
    // Asset used by the other suites: info == the definition.
    for (u32 c = 0; c < A.big.mesh.dag.cluster_count(); ++c) {
        const dag::DagClusterLink& l = A.big.mesh.dag.links[c];
        const bool ok = A.big.info[c].member_page == A.big.pages.group_page[l.group] &&
                        (l.refined == dag::kDagNoGroup ? A.big.info[c].producer_page == gs::kPageNone
                                                       : A.big.info[c].producer_page == A.big.pages.group_page[l.refined]);
        if (!ok) {
            expect(false, "StreamClusterInfo of cluster " + std::to_string(c));
            break;
        }
    }
}

// --- format ------------------------------------------------------------------------------------------
void testFormat() {
    Assets& A = assets();
    if (!A.ok) {
        expect(false, "assets");
        return;
    }
    const gs::ClusterPageFile& f = A.medium.pages;
    const std::vector<u8> bytes = gs::serialize_cluster_page_file(f);
    gs::ClusterPageFile back;
    std::string err;
    expect(gs::parse_cluster_page_file(bytes.data(), bytes.size(), back, &err), "parse: " + err);
    expect(gs::cluster_page_file_equal(f, back), "round trip bit for bit");
    expect(gs::serialize_cluster_page_file(back) == bytes, "re-serialised bytes identical");
    gs::ClusterPageFile again;
    expect(gs::build_cluster_pages(A.medium.mesh, gs::PageBuildOptions{32u * 1024u}, again, &err) &&
               gs::serialize_cluster_page_file(again) == bytes,
           "deterministic bytes");
    u32 truncRejected = 0, truncTried = 0;
    for (usize n = 0; n < bytes.size(); n += (n < 512u ? 1u : 97u)) {
        ++truncTried;
        truncRejected += gs::parse_cluster_page_file(bytes.data(), n, back) ? 0u : 1u;
    }
    expect(truncRejected == truncTried, "every truncation rejected");
    std::mt19937 rng(7);
    u32 flipsRejected = 0;
    for (u32 i = 0; i < 300u; ++i) {
        std::vector<u8> b = bytes;
        const usize at = rng() % b.size();
        b[at] ^= static_cast<u8>(1u << (rng() % 8u));
        flipsRejected += gs::parse_cluster_page_file(b.data(), b.size(), back) ? 0u : 1u;
    }
    expect(flipsRejected == 300u, "every bit flip rejected (checksum)");
    // Structural corruptions with a valid checksum.
    auto reseal = [](std::vector<u8>& b) {
        const u64 h = geometry::meshlet_fnv1a64(b.data(), b.size() - 8u);
        std::memcpy(b.data() + b.size() - 8u, &h, 8u);
    };
    auto mutate = [&](usize offset, u32 value) {
        std::vector<u8> b = bytes;
        std::memcpy(b.data() + offset, &value, 4u);
        reseal(b);
        return gs::parse_cluster_page_file(b.data(), b.size(), back);
    };
    const usize table = gs::kClusterPageHeaderBytes;
    const usize groupsAt = table + f.pages.size() * sizeof(gs::ClusterPageEntry);
    const usize depsAt = groupsAt + f.page_groups.size() * 4u;
    const usize groupPageAt = depsAt + f.page_deps.size() * 4u;
    u32 lastPageWithDeps = 0;
    for (u32 p = 0; p < f.page_count(); ++p) {
        lastPageWithDeps = f.pages[p].dep_count > 0u ? p : lastPageWithDeps;
    }
    const gs::ClusterPageEntry& e = f.pages[lastPageWithDeps];
    struct Case {
        const char* what;
        usize offset;
        u32 value;
    } cases[] = {
        {"magic", 0u, 0x12345678u},
        {"major version", 4u, 2u},
        {"header bytes", 8u, 64u},
        {"reserved", 112u, 1u},
        {"coarse page count 0", 28u, 0u},
        {"page group count 0", table + 16u, 0u},
        {"dependency on itself", depsAt + e.dep_offset * 4u, lastPageWithDeps},
        {"group_page mismatch", groupPageAt, 1u},
        {"duplicate group", groupsAt + 4u, f.page_groups[0]},
        {"coarse flag", table + lastPageWithDeps * sizeof(gs::ClusterPageEntry) + 36u, gs::kPageFlagCoarse},
        {"payload bytes over capacity", table + 8u, f.page_bytes + 16u},
        {"payload magic", 0u, 0u}, // patched below (payload offset)
    };
    u64 payloadOffset = 0;
    std::memcpy(&payloadOffset, bytes.data() + 96u, 8u);
    cases[11].offset = static_cast<usize>(payloadOffset);
    for (const Case& c : cases) {
        expect(!mutate(c.offset, c.value), std::string("corruption rejected: ") + c.what);
    }
    // Unknown minor accepted.
    {
        std::vector<u8> b = bytes;
        const u16 minor = 7u;
        std::memcpy(b.data() + 6u, &minor, 2u);
        reseal(b);
        expect(gs::parse_cluster_page_file(b.data(), b.size(), back), "newer minor accepted");
    }
    // Validation against the wrong DAG.
    expect(!gs::validate_cluster_page_file(A.medium.pages, A.small.mesh, &err), "file rejected against another DAG");
    // File I/O.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_rp_geometry_streaming";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "medium.fusepages").string();
    expect(gs::write_cluster_page_file(path, f, &err), "write: " + err);
    expect(gs::load_cluster_page_file(path, back, &err) && gs::cluster_page_file_equal(f, back), "load: " + err);
    std::filesystem::remove_all(dir);
    std::printf("format: %zu bytes, %u truncations, 300 flips, %zu corruptions rejected\n", bytes.size(), truncTried,
                sizeof(cases) / sizeof(cases[0]));
}

// --- kernel ------------------------------------------------------------------------------------------
/// Random residency closed under dependencies: pick pages, add their closure; coarse always.
std::vector<u32> closedResidency(const gs::ClusterPageFile& f, std::mt19937& rng, u32 picks) {
    std::vector<u8> in(f.page_count(), 0u);
    std::vector<u32> stack;
    for (u32 p = 0; p < f.coarse_page_count; ++p) {
        in[p] = 1u;
    }
    for (u32 k = 0; k < picks; ++k) {
        stack.push_back(static_cast<u32>(rng() % f.page_count()));
    }
    while (!stack.empty()) {
        const u32 p = stack.back();
        stack.pop_back();
        if (in[p] != 0u && p >= f.coarse_page_count) {
            continue;
        }
        in[p] = 1u;
        const gs::ClusterPageEntry& e = f.pages[p];
        for (u32 i = 0; i < e.dep_count; ++i) {
            if (in[f.page_deps[e.dep_offset + i]] == 0u) {
                stack.push_back(f.page_deps[e.dep_offset + i]);
            }
        }
    }
    std::vector<u32> bits((f.page_count() + 31u) / 32u, 0u);
    for (u32 p = 0; p < f.page_count(); ++p) {
        if (in[p] != 0u) {
            bits[p >> 5u] |= 1u << (p & 31u);
        }
    }
    return bits;
}

bool bit(const std::vector<u32>& bits, u32 p) { return p != gs::kPageNone && ((bits[p >> 5u] >> (p & 31u)) & 1u) != 0u; }

std::set<u32> listSet(const std::vector<u32>& words, u32 offset, u32 count) {
    return std::set<u32>(words.begin() + offset, words.begin() + offset + count);
}

void testKernel() {
    Assets& A = assets();
    if (!A.ok) {
        expect(false, "assets");
        return;
    }
    std::mt19937 rng(1234);
    for (const StreamAsset* a : {&A.big, &A.medium}) {
        const u32 pages = a->pages.page_count();
        const f64 area = leafArea(*a);
        const f32 radius = a == &A.big ? 10.f : 3.f;
        const std::vector<u32> all = allResident(pages);
        u32 idealEqual = 0, parityEqual = 0;
        for (u32 i = 0; i < 400u; ++i) {
            const dag::cut_kernel::DagView v = randomView(rng, radius);
            KernelRun run, ref;
            runKernel(*a, all, v, run);
            std::vector<u32> ideal;
            dag::evaluate_dag_cut(a->mesh.dag, v, ideal);
            idealEqual += run.cut == ideal ? 1u : 0u;
            runKernel(*a, all, v, ref, Backend::CpuReference);
            const sk::FeedbackLayout l = sk::feedback_layout(pages);
            const bool same = ref.cut == run.cut &&
                              std::equal(ref.feedback.begin(), ref.feedback.begin() + l.list, run.feedback.begin()) &&
                              listSet(ref.feedback, l.list, ref.feedback[0]) == listSet(run.feedback, l.list, run.feedback[0]) &&
                              listSet(ref.draw, 0, ref.feedback[1]) == listSet(run.draw, 0, run.feedback[1]);
            parityEqual += same ? 1u : 0u;
            // Every page resident: no refine requests; exactly the member pages of drawn clusters are kept,
            // each with the highest priority of its drawn clusters' groups.
            std::vector<u32> keep(pages, 0u);
            for (u32 c = 0; c < run.cut.size(); ++c) {
                if (run.cut[c] != 0u) {
                    const u32 m = a->info[c].member_page;
                    keep[m] = std::max(keep[m], sk::refine_priority(a->mesh.dag.links[c].parent, v));
                }
            }
            if (!std::equal(keep.begin(), keep.end(), run.feedback.begin() + l.priorities)) {
                expect(false, "all resident: feedback == keep priorities of the drawn clusters' pages");
            }
        }
        expect(idealEqual == 400u, "all pages resident: stream cut == geometry_dag_cut (" + std::to_string(idealEqual) + "/400)");
        expect(parityEqual == 400u, "CpuReference == CpuParallel (" + std::to_string(parityEqual) + "/400)");

        // Random closed residency states.
        u32 states = 0, requests = 0, maxPriority = 0, openCuts = 0, badPages = 0, badArea = 0;
        f64 minRatio = 10.0, maxRatio = 0.0;
        for (u32 i = 0; i < 300u; ++i) {
            const std::vector<u32> bits = closedResidency(a->pages, rng, rng() % 12u);
            const dag::cut_kernel::DagView v = randomView(rng, radius);
            KernelRun run;
            runKernel(*a, bits, v, run);
            ++states;
            const CutCheck chk = check_cut(*a, run.cut, [&](u32 p) { return bit(bits, p) ? a->pages.page_payload(p) : nullptr; });
            openCuts += chk.open_edges != 0u ? 1u : 0u;
            badPages += chk.pages_ok ? 0u : 1u;
            const f64 ratio = chk.area / area;
            minRatio = std::min(minRatio, ratio);
            maxRatio = std::max(maxRatio, ratio);
            badArea += (ratio < 0.7 || ratio > 1.2) ? 1u : 0u;
            const sk::FeedbackLayout l = sk::feedback_layout(pages);
            // Expected feedback, from the definition: keep = member pages of drawn clusters; refine = missing
            // producer pages of drawn clusters whose own LOD is not acceptable.
            std::vector<u32> expectPrio(pages, 0u);
            for (u32 c = 0; c < run.cut.size(); ++c) {
                if (run.cut[c] == 0u) {
                    continue;
                }
                const dag::DagClusterLink& link = a->mesh.dag.links[c];
                const gs::StreamClusterInfo& in = a->info[c];
                expect(bit(bits, in.member_page), "drawn clusters live in resident pages");
                expectPrio[in.member_page] = std::max(expectPrio[in.member_page], sk::refine_priority(link.parent, v));
                if (!dag::cut_kernel::lod_acceptable(link.self, v) && in.producer_page != gs::kPageNone) {
                    expect(!bit(bits, in.producer_page), "a non-acceptable drawn cluster's producer page is missing");
                    expectPrio[in.producer_page] = std::max(expectPrio[in.producer_page], sk::refine_priority(link.self, v));
                }
            }
            if (!std::equal(expectPrio.begin(), expectPrio.end(), run.feedback.begin() + l.priorities)) {
                expect(false, "feedback priorities == definition");
            }
            for (u32 p = 0; p < pages; ++p) {
                const u32 prio = run.feedback[l.priorities + p];
                if (prio != 0u) {
                    expect(prio > sk::kPriorityMin && prio <= sk::kPriorityMin + sk::kPriorityLevels, "priority range");
                }
                if (prio != 0u && !bit(bits, p)) {
                    ++requests;
                    maxPriority = std::max(maxPriority, prio);
                }
            }
            expect(run.feedback[0] == static_cast<u32>(listSet(run.feedback, l.list, run.feedback[0]).size()), "request list unique");
        }
        expect(openCuts == 0u, "closed residency: every cut watertight (" + std::to_string(openCuts) + " open)");
        expect(badPages == 0u, "closed residency: drawn clusters only from resident pages");
        expect(badArea == 0u, "closed residency: cut area within [0.7, 1.2] of the source");
        expect(requests > 100u && maxPriority > 3u, "refine requests happen, priorities spread");
        std::printf("kernel: %u pages: 400 views == WP-5.2 cut and CpuReference; %u closed residency states: watertight, area "
                    "%.3f..%.3f, %u refine requests (max priority %u)\n",
                    pages, states, minRatio, maxRatio, requests, maxPriority);

        // Priority monotone in the projected error: a closer camera never lowers a bound's priority.
        for (u32 c = 0; c < a->mesh.dag.cluster_count(); c += 7u) {
            const dag::DagLodBounds& b = a->mesh.dag.links[c].self;
            if (!(b.error > 0.f) || !(b.error < dag::kDagErrorTerminal)) {
                continue;
            }
            u32 prev = 0;
            for (u32 s = 0; s < 12u; ++s) {
                const f32 d = b.radius + 40.f * radius / static_cast<f32>(1u << s);
                const f32 cam[3] = {b.center[0] + d, b.center[1], b.center[2]};
                const dag::cut_kernel::DagView v = make_view(cam, 1.f);
                if (dag::cut_kernel::lod_acceptable(b, v)) {
                    continue;
                }
                const u32 prio = sk::refine_priority(b, v);
                expect(prio >= prev && prio >= 2u, "priority non-decreasing as the camera approaches");
                prev = prio;
            }
        }
    }

    // Mutation: residency that breaks the dependency closure cracks the cut.
    {
        const StreamAsset& a = A.big;
        u32 cracked = 0, tries = 0;
        for (; tries < 400u && cracked == 0u; ++tries) {
            std::vector<u32> bits = closedResidency(a.pages, rng, 6u + rng() % 10u);
            // Drop one resident non-coarse page that has a resident dependent.
            std::vector<u32> victims;
            for (u32 p = a.pages.coarse_page_count; p < a.pages.page_count(); ++p) {
                const gs::ClusterPageEntry& e = a.pages.pages[p];
                if (!bit(bits, p)) {
                    continue;
                }
                for (u32 i = 0; i < e.dep_count; ++i) {
                    victims.push_back(a.pages.page_deps[e.dep_offset + i]);
                }
            }
            if (victims.empty()) {
                continue;
            }
            const u32 drop = victims[rng() % victims.size()];
            if (drop < a.pages.coarse_page_count) {
                continue;
            }
            bits[drop >> 5u] &= ~(1u << (drop & 31u));
            const f32 cam[3] = {0.f, 0.f, 10.3f};
            for (f32 thr : {0.1f, 0.5f, 2.f}) {
                KernelRun run;
                runKernel(a, bits, make_view(cam, thr), run);
                const CutCheck chk = check_cut(a, run.cut, [&](u32 p) { return a.pages.page_payload(p); });
                cracked += chk.open_edges != 0u ? 1u : 0u;
            }
        }
        expect(cracked > 0u, "mutation: a residency state that breaks the closure cracks some cut");
        std::printf("kernel mutation: closure broken -> cracked cut found after %u state(s)\n", tries);
    }
}

// --- priority ----------------------------------------------------------------------------------------
void testPriority() {
    Assets& A = assets();
    if (!A.ok) {
        expect(false, "assets");
        return;
    }
    const StreamAsset& a = A.big;
    gs::ClusterStreamer s;
    gs::StreamerDesc d{};
    d.budget_pages = a.pages.page_count();
    d.max_loads_per_frame = 1u;
    std::string err;
    expect(s.init(a.pages, d, &err), "streamer init: " + err);
    u32 frame = 1;
    while (!s.coarse_resident() && frame < 1000u) {
        s.begin_frame(frame, frame - 1u);
        s.update();
        for (u32 i = 0; i < s.load_count(); ++i) {
            expect(s.load_page(i) < a.pages.coarse_page_count, "coarse pages load first");
            s.complete_load(s.load_page(i));
        }
        ++frame;
    }
    expect(s.coarse_resident() && frame - 1u == a.pages.coarse_page_count, "one coarse page per frame with max_loads 1");
    // Close camera: many refine requests with different priorities; each frame loads the most urgent ready one.
    const f32 cam[3] = {0.f, 0.f, 10.4f};
    const dag::cut_kernel::DagView v = make_view(cam, 1.f);
    u32 ordered = 0, frames = 0;
    for (u32 k = 0; k < 40u; ++k, ++frame) {
        std::vector<u32> bits(s.resident_bits(), s.resident_bits() + s.resident_words());
        KernelRun run;
        runKernel(a, bits, v, run);
        s.begin_frame(frame, frame - 1u);
        s.apply_feedback(run.feedback.data(), static_cast<u32>(run.feedback.size()));
        s.update();
        if (s.load_count() == 0u) {
            break;
        }
        ++frames;
        const u32 loaded = s.load_page(0);
        // The loaded page has the highest priority among the requested pages whose dependencies are resident.
        u32 best = 0;
        const sk::FeedbackLayout l = sk::feedback_layout(a.pages.page_count());
        for (u32 p = 0; p < a.pages.page_count(); ++p) {
            if (!s.page_resident(p) && s.residency().requested(p) && s.residency().deps_resident(p)) {
                best = std::max(best, s.residency().priority(p));
            }
        }
        (void)l;
        ordered += s.residency().priority(loaded) == best ? 1u : 0u;
        expect(s.residency().deps_resident(loaded), "a page loads after its dependencies");
        s.complete_load(loaded);
    }
    expect(frames > 5u && ordered == frames, "each load is the most urgent ready page (" + std::to_string(ordered) + "/" +
                                                 std::to_string(frames) + ")");
    std::printf("priority: %u single-load frames, every load the highest-priority ready page\n", frames);
}

// --- fly-through -------------------------------------------------------------------------------------
struct FlyConfig {
    const char* name;
    u32 budgetPages;      ///< 0: every page
    u32 shrinkAt;         ///< frame at which the budget shrinks (0: never)
    u32 shrinkTo;
    bool expectConverge;
};

struct FlyResult {
    u32 frames = 0, missing = 0, coarseMissing = 0, overBudget = 0, degraded = 0;
    u32 loads = 0, evictions = 0, failures = 0, slotFailures = 0, maxResident = 0;
    f64 minArea = 10.0, maxArea = 0.0;
    bool converged = false;
    u64 drawn = 0, idealDrawn = 0; ///< triangles over all frames: resident cut / ideal cut
    unsigned long long allocations = 0;
};

FlyResult flythrough(const StreamAsset& a, const FlyConfig& cfg, bool countAllocations) {
    FlyResult r;
    const u32 pages = a.pages.page_count();
    const u32 pageBytes = a.pages.page_bytes;
    gs::ClusterStreamer s;
    gs::StreamerDesc d{};
    d.budget_pages = cfg.budgetPages == 0u ? pages : cfg.budgetPages;
    d.reserve_slots = 8u;
    d.max_loads_per_frame = 12u;
    std::string err;
    if (!s.init(a.pages, d, &err)) {
        expect(false, std::string(cfg.name) + ": streamer init: " + err);
        return r;
    }
    // Host "pool": slot_count slots of page_bytes (16-aligned storage).
    std::vector<fuse::u64> poolStorage(static_cast<usize>(s.slot_count()) * pageBytes / 8u + 2u, 0u);
    u8* pool = reinterpret_cast<u8*>(poolStorage.data());
    auto slotBytes = [&](u32 slot) { return pool + static_cast<usize>(slot) * pageBytes; };
    auto residentData = [&](u32 page) -> const u8* {
        const u32 slot = s.resident_slot(page);
        return slot == gs::kPageNone ? nullptr : slotBytes(slot);
    };
    struct InFlight {
        u32 page, due;
    };
    std::vector<InFlight> inflight;
    inflight.reserve(pages);
    std::mt19937 rng(99);
    // Prime the coarse LOD (synchronous, before the first frame).
    u32 frame = 1;
    while (!s.coarse_resident() && frame < 64u) {
        s.begin_frame(frame, frame - 1u);
        s.update();
        for (u32 i = 0; i < s.load_count(); ++i) {
            std::memcpy(slotBytes(s.load_slot(i)), a.pages.page_payload(s.load_page(i)), a.pages.pages[s.load_page(i)].payload_bytes);
            s.complete_load(s.load_page(i));
        }
        ++frame;
    }
    const f64 area = leafArea(a);
    constexpr u32 kFrames = 300u;
    constexpr u32 kHold = 60u;
    constexpr u32 kWarmup = 20u;
    KernelRun run;
    std::vector<u32> feedback(sk::feedback_layout(pages).words, 0u);
    std::vector<u32> bits(s.resident_words(), 0u);
    std::vector<u32> ideal;
    std::vector<u8> live(s.slot_count(), 0u);
    for (u32 f = 0; f < kFrames; ++f, ++frame) {
        const bool measure = countAllocations && f >= kWarmup;
        if (cfg.shrinkAt != 0u && f == cfg.shrinkAt) {
            expect(s.set_budget_pages(cfg.shrinkTo), std::string(cfg.name) + ": budget shrink");
        }
        t_allocations = 0;
        t_count = measure;
        s.begin_frame(frame, frame - 1u);
        // IO completion (latency 2 frames, 3% failures).
        usize keep = 0;
        for (usize i = 0; i < inflight.size(); ++i) {
            if (inflight[i].due > f) {
                inflight[keep++] = inflight[i];
                continue;
            }
            if (rng() % 100u < 3u) {
                s.fail_load(inflight[i].page);
                ++r.failures;
            } else {
                s.complete_load(inflight[i].page);
            }
        }
        inflight.resize(keep);
        s.apply_feedback(feedback.data(), static_cast<u32>(feedback.size()));
        const gs::StreamerStats& st = s.update();
        r.loads += st.loads;
        r.evictions += st.evictions;
        r.slotFailures += st.slot_failures;
        t_count = false;
        for (u32 i = 0; i < s.load_count(); ++i) {
            const u32 page = s.load_page(i);
            std::memcpy(slotBytes(s.load_slot(i)), a.pages.page_payload(page), a.pages.pages[page].payload_bytes);
            inflight.push_back(InFlight{page, f + 2u});
        }
        // Poison every slot that holds neither a resident nor a loading page (evicted pages' bytes are gone:
        // a cut that still used them would decode garbage and fail the checks below).
        std::fill(live.begin(), live.end(), u8{0});
        for (u32 p = 0; p < pages; ++p) {
            const fuse::core_logic::PageState ps = s.residency().state(p);
            if (ps == fuse::core_logic::PageState::Loading || ps == fuse::core_logic::PageState::Resident) {
                live[s.assigned_slot(p)] = 1u;
            }
        }
        for (u32 slot = 0; slot < s.slot_count(); ++slot) {
            if (live[slot] == 0u) {
                std::memset(slotBytes(slot), 0xCD, pageBytes);
            }
        }
        // The cut of this frame, from this frame's resident set.
        std::copy(s.resident_bits(), s.resident_bits() + s.resident_words(), bits.begin());
        f32 cam[3];
        flythrough_camera(f, kFrames, kHold, 10.f, cam);
        const dag::cut_kernel::DagView v = make_view(cam, 1.f);
        t_count = measure;
        runKernel(a, bits, v, run);
        std::copy(run.feedback.begin(), run.feedback.end(), feedback.begin());
        t_count = false;
        r.allocations += measure ? t_allocations : 0u;
        // Checks: every drawn cluster decoded from resident slot bytes only.
        const CutCheck chk = check_cut(a, run.cut, residentData);
        const f64 ratio = chk.area / area;
        r.minArea = std::min(r.minArea, ratio);
        r.maxArea = std::max(r.maxArea, ratio);
        const bool coarseOk = s.coarse_resident();
        const bool missing = !chk.pages_ok || chk.open_edges != 0u || ratio < 0.7 || ratio > 1.2 || chk.clusters == 0u;
        r.missing += missing ? 1u : 0u;
        r.coarseMissing += coarseOk ? 0u : 1u;
        const u64 used = s.residency().resident_bytes() + s.residency().loading_bytes();
        r.overBudget += used > static_cast<u64>(s.budget_pages()) * pageBytes ? 1u : 0u;
        r.maxResident = std::max(r.maxResident, s.resident_page_count() + s.loading_page_count());
        dag::evaluate_dag_cut(a.mesh.dag, v, ideal);
        const bool isIdeal = ideal == run.cut;
        r.drawn += chk.triangles;
        for (u32 c = 0; c < ideal.size(); ++c) {
            r.idealDrawn += ideal[c] != 0u ? geometry::dag::cluster_ref(a.mesh, c).record->triangle_count : 0u;
        }
        r.degraded += isIdeal ? 0u : 1u;
        if (f + 1u == kFrames) {
            r.converged = isIdeal;
        }
        if (missing && r.missing <= 3u) {
            std::fprintf(stderr, "  %s frame %u: pages_ok %d, open edges %llu, area ratio %.3f, clusters %u\n", cfg.name, f,
                         chk.pages_ok ? 1 : 0, static_cast<unsigned long long>(chk.open_edges), ratio, chk.clusters);
        }
        ++r.frames;
    }
    return r;
}

void testFlythrough() {
    Assets& A = assets();
    if (!A.ok) {
        expect(false, "assets");
        return;
    }
    const StreamAsset& a = A.big;
    const u32 pages = a.pages.page_count();
    const u32 coarse = a.pages.coarse_page_count;
    const FlyConfig configs[] = {
        {"generous", 0u, 0u, 0u, true},
        {"tight", coarse + pages / 3u, 0u, 0u, false},
        {"shrinking", (2u * pages) / 3u, 150u, coarse + pages / 8u, false},
        {"starved", coarse + pages / 8u, 0u, 0u, false},
    };
    for (const FlyConfig& cfg : configs) {
        const FlyResult r = flythrough(a, cfg, false);
        std::printf("flythrough %-9s budget %4u/%u pages: %u frames, %u missing geometry, %u missing coarse, %u over budget, "
                    "%u degraded (streaming lag / budget), area %.3f..%.3f, %u loads, %u evictions, %u failed loads, peak %u pages, "
                    "triangles drawn %.1f%% of the ideal cut's%s\n",
                    cfg.name, cfg.budgetPages == 0u ? pages : cfg.budgetPages, pages, r.frames, r.missing, r.coarseMissing,
                    r.overBudget, r.degraded, r.minArea, r.maxArea, r.loads, r.evictions, r.failures, r.maxResident,
                    100.0 * static_cast<f64>(r.drawn) / static_cast<f64>(std::max<u64>(r.idealDrawn, 1u)),
                    r.converged ? ", converged to the ideal cut" : "");
        const std::string n = cfg.name;
        expect(r.frames == 300u, n + ": all frames ran");
        expect(r.missing == 0u, n + ": 0 frames missing geometry");
        expect(r.coarseMissing == 0u, n + ": coarse LOD resident on every frame");
        expect(r.overBudget == 0u, n + ": budget never exceeded");
        expect(r.loads > 20u, n + ": pages stream in");
        if (cfg.expectConverge) {
            expect(r.converged, n + ": converges to the ideal cut once the camera holds still");
        } else {
            expect(r.evictions > 0u, n + ": the budget forces evictions");
        }
    }
}

void testZeroAlloc() {
    Assets& A = assets();
    if (!A.ok) {
        expect(false, "assets");
        return;
    }
    const StreamAsset& a = A.big;
    const FlyConfig cfg{"zero_alloc", a.pages.coarse_page_count + a.pages.page_count() / 3u, 0u, 0u, false};
    const FlyResult r = flythrough(a, cfg, true);
    std::printf("zero_alloc: %u steady-state frames (tight budget, %u loads, %u evictions): %llu operator-new calls in "
                "ClusterStreamer + geometry_stream_cut launches\n",
                r.frames - 20u, r.loads, r.evictions, r.allocations);
    expect(r.allocations == 0u, "streamer + kernel make no steady-state heap allocations");
    expect(r.missing == 0u, "zero_alloc run: no frame missing geometry");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    bool ran = false;
    struct Suite {
        const char* name;
        void (*fn)();
    } suites[] = {{"layout", testLayout},   {"format", testFormat},         {"kernel", testKernel},
                  {"priority", testPriority}, {"flythrough", testFlythrough}, {"zero_alloc", testZeroAlloc}};
    for (const Suite& s : suites) {
        if (all || suite == s.name) {
            ran = true;
            s.fn();
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
