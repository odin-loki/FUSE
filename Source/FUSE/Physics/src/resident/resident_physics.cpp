// Resident physics pipeline: host orchestration (see resident_physics.hpp / resident_kernels.hpp).

#include <fuse/physics/resident/resident_physics.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/physics/broadphase/broadphase_kernel.hpp>
#include <fuse/physics/broadphase/radix_sort_launch.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>
#include <fuse/physics/resident/resident_cuda_backend.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace fuse::physics::resident {

namespace bk = broadphase_kernel;

namespace {

/// Grow policy: the live size plus a quarter of headroom, rounded to a 1024-element tile.
u32 grownCapacity(u32 needed) {
    const u64 grown = static_cast<u64>(needed) + needed / 4u + 1024u;
    const u64 rounded = (grown + 1023u) / 1024u * 1024u;
    return static_cast<u32>(std::min<u64>(rounded, 0xFFFFF000ull));
}

enum class PairSource : u8 { None, Broadphase, Uploaded };

} // namespace

struct ResidentPhysics::Impl {
    Space space = Space::Host;
    kernel::Backend hostBackend = kernel::Backend::CpuParallel;
    void* stream = nullptr;
    void* eventStart = nullptr;
    void* eventStop = nullptr;
    std::chrono::steady_clock::time_point hostStart{};
#if defined(FUSE_HAS_CUDA)
    cuda_backend::Entries entries{};
#endif

    // ---- memory ------------------------------------------------------------------------------

    struct Block {
        void* ptr = nullptr;
        usize bytes = 0;
    };

    template <typename T>
    struct Buffer {
        Block block;
        T* data() const { return static_cast<T*>(block.ptr); }
        usize count() const { return block.bytes / sizeof(T); }
    };

    bool allocBlock(Block& b, usize bytes) {
        if (bytes <= b.bytes && b.ptr != nullptr) {
            return true;
        }
        freeBlock(b);
        if (bytes == 0u) {
            return true;
        }
        void* p = nullptr;
        if (space == Space::Host) {
            p = std::malloc(bytes);
        }
#if defined(FUSE_HAS_CUDA)
        else if (!cuda_backend::allocate(&p, bytes)) {
            p = nullptr;
        }
#endif
        if (p == nullptr) {
            return false;
        }
        b.ptr = p;
        b.bytes = bytes;
        return true;
    }

    void freeBlock(Block& b) {
        if (b.ptr != nullptr) {
            if (space == Space::Host) {
                std::free(b.ptr);
            }
#if defined(FUSE_HAS_CUDA)
            else {
                cuda_backend::release(b.ptr);
            }
#endif
        }
        b.ptr = nullptr;
        b.bytes = 0u;
    }

    /// Grow-only (contents are not kept); at least one element so every pointer is valid.
    template <typename T>
    bool reserve(Buffer<T>& b, usize count) {
        return allocBlock(b.block, std::max<usize>(count, 1u) * sizeof(T));
    }

    bool copy(void* dst, const void* src, usize bytes, cuda_backend::CopyKind kind) {
        if (bytes == 0u) {
            return true;
        }
        if (space == Space::Host) {
            std::memcpy(dst, src, bytes);
            return true;
        }
#if defined(FUSE_HAS_CUDA)
        return cuda_backend::copyAsync(dst, src, bytes, kind, stream);
#else
        (void)kind;
        return false;
#endif
    }

    template <typename T>
    bool upload(Buffer<T>& b, const T* src, usize count) {
        return reserve(b, count) && copy(b.data(), src, count * sizeof(T), cuda_backend::CopyKind::HostToDevice);
    }

    template <typename T>
    bool download(T* dst, const T* src, usize count) {
        return copy(dst, src, count * sizeof(T), cuda_backend::CopyKind::DeviceToHost);
    }

    bool fill(void* dst, int value, usize bytes) {
        if (bytes == 0u) {
            return true;
        }
        if (space == Space::Host) {
            std::memset(dst, value, bytes);
            return true;
        }
#if defined(FUSE_HAS_CUDA)
        return cuda_backend::fillAsync(dst, value, bytes, stream);
#else
        return false;
#endif
    }

    bool sync() {
        if (space == Space::Host) {
            return true;
        }
#if defined(FUSE_HAS_CUDA)
        return cuda_backend::synchronize(stream);
#else
        return false;
#endif
    }

    // ---- launches ------------------------------------------------------------------------------

    kernel::Backend backend() const { return space == Space::Cuda ? kernel::Backend::Cuda : hostBackend; }

    bk::SortEntries sortEntries() const {
        bk::SortEntries e{};
#if defined(FUSE_HAS_CUDA)
        if (space == Space::Cuda) {
            e = entries.sort;
            e.stream = stream;
            e.synchronize = false;
        }
#endif
        return e;
    }

    template <typename Body, typename Params>
    bool launch(const char* name, u32 grid, kernel::DeviceEntryFn entry, const Params& params,
                kernel::Dim3 workgroup = kItemWorkgroup) {
        if (grid == 0u) {
            return true;
        }
        kernel::LaunchOptions options{};
        if (space == Space::Cuda) {
            options.cuda = entry;
            options.stream = stream;
            options.synchronize = false;
            options.allow_fallback = false;
        }
        return kernel::launch(backend(), kernel::KernelLaunch{name, kernel::extent1(grid), workgroup}, Body{}, params,
                              options)
            .ok;
    }

    /// Exclusive scan of data[0, count) in place; returns the device address of the total.
    const u32* scan(u32* data, u32 count, u32* levelStorage, bool& ok) {
        const bk::ScanPlan plan = bk::planScan(count);
        ok = ok && bk::launchScan(backend(), data, levelStorage, plan, sortEntries());
        return levelStorage + plan.offsets[plan.levels];
    }

    // ---- scene -----------------------------------------------------------------------------------

    u32 bodyCount = 0;
    u32 orientationCount = 0;
    u32 layerCount = 0;
    u32 shapeCount = 0;
    u32 planeCount = 0;
    Buffer<vec3> positions;
    Buffer<quat> orientations;
    Buffer<u32> flags;
    Buffer<f32> invMasses;
    Buffer<f32> frictionStatic;
    Buffer<u32> layers;
    Buffer<u32> masks;
    Buffer<u32> shapeTypes;
    Buffer<vec3> shapeParams;
    Buffer<f32> shapeScalars;
    Buffer<u32> shapeBodies;
    Buffer<u32> bodyShape;
    Buffer<u32> planeBodies;
    std::vector<u32> hostScratch;
    bool sceneReady = false;

    // ---- broadphase ------------------------------------------------------------------------------

    u32 entryCap = 0;
    u32 pairCap = 0;
    Buffer<u32> shapeCounts;
    Buffer<u32> entryKeys;
    Buffer<u32> entryBodies;
    Buffer<u32> entryKeysTmp;
    Buffer<u32> entryBodiesTmp;
    Buffer<u32> sortHist;
    Buffer<u32> runIndex;
    Buffer<u32> runStarts;
    Buffer<u32> uniqueCounts;
    Buffer<u32> pairOffsets;
    Buffer<u32> pairKeys;
    Buffer<u32> pairKeysTmp;
    Buffer<u32> uniqueIndex;
    Buffer<u32> pairs; ///< narrowphase input: unique broadphase keys or uploaded pairs
    Buffer<u32> levels;
    Buffer<u32> counters; ///< [0] clipped writes, [1] zero (empty-scene totals)
    Buffer<Status> status;
    u32 regionShape = 0;
    u32 regionRun = 0;
    u32 regionPairCount = 0;
    u32 regionUnique = 0;
    u32 regionSort = 0;
    u32 regionNarrow = 0;
    usize levelShapes = 0; ///< shapeCount the level layout was planned for
    usize narrowGrid = 0;  ///< narrowphase grid the level layout was planned for

    PairSource pairSource = PairSource::None;
    u32 pairBits = 16;
    u32 uploadedPairCount = 0;
    const u32* pairTotal = nullptr; ///< device total of the broadphase pairs
    std::vector<u32> packedPairs;

    // ---- narrowphase -----------------------------------------------------------------------------

    u32 contactCap = 0;
    Buffer<narrowphase::ContactManifold> staging;
    Buffer<u32> npFlags;
    Buffer<u32> npOffsets;
    Buffer<narrowphase::ContactManifold> contacts;

    // ---- solver ----------------------------------------------------------------------------------

    u32 solverBodies = 0;
    u32 solverContacts = 0;
    u32 solverDistances = 0;
    u32 invInertiaCount = 0;
    u32 anchorCount = 0;
    u32 contactLambdaCount = 0;
    u32 pointLambdaCount = 0;
    u32 distanceLambdaCount = 0;
    u32 colorStart[ConstraintColoring::kMaxColors + 2u]{};
    u32 colorCount = 0;
    u32 overflowCount = 0;
    Buffer<vec3> predictedPositions;
    Buffer<quat> predictedOrientations;
    Buffer<u32> solverFlags;
    Buffer<f32> solverInvMasses;
    Buffer<f32> solverFriction;
    Buffer<vec3> invInertia;
    Buffer<narrowphase::ContactManifold> solverContactBuffer;
    Buffer<ContactAnchor> anchors;
    Buffer<f32> contactLambdas;
    Buffer<f32> pointLambdas;
    Buffer<DistanceConstraint> distances;
    Buffer<f32> distanceLambdas;
    Buffer<u32> colorItems;
    bool solverReady = false;

    ~Impl() { releaseAll(); }

    void releaseAll() {
        Block* blocks[] = {&positions.block, &orientations.block, &flags.block, &invMasses.block,
                           &frictionStatic.block, &layers.block, &masks.block, &shapeTypes.block,
                           &shapeParams.block, &shapeScalars.block, &shapeBodies.block, &bodyShape.block,
                           &planeBodies.block, &shapeCounts.block, &entryKeys.block, &entryBodies.block,
                           &entryKeysTmp.block, &entryBodiesTmp.block, &sortHist.block, &runIndex.block,
                           &runStarts.block, &uniqueCounts.block, &pairOffsets.block, &pairKeys.block,
                           &pairKeysTmp.block, &uniqueIndex.block, &pairs.block, &levels.block,
                           &counters.block, &status.block, &staging.block, &npFlags.block, &npOffsets.block,
                           &contacts.block, &predictedPositions.block, &predictedOrientations.block,
                           &solverFlags.block, &solverInvMasses.block, &solverFriction.block, &invInertia.block,
                           &solverContactBuffer.block, &anchors.block, &contactLambdas.block,
                           &pointLambdas.block, &distances.block, &distanceLambdas.block, &colorItems.block};
        for (Block* b : blocks) {
            freeBlock(*b);
        }
#if defined(FUSE_HAS_CUDA)
        if (space == Space::Cuda) {
            if (eventStart != nullptr) {
                cuda_backend::destroyEvent(eventStart);
            }
            if (eventStop != nullptr) {
                cuda_backend::destroyEvent(eventStop);
            }
            if (stream != nullptr) {
                cuda_backend::destroyStream(stream);
            }
        }
#endif
        eventStart = nullptr;
        eventStop = nullptr;
        stream = nullptr;
        entryCap = pairCap = contactCap = 0u;
        levelShapes = narrowGrid = 0u;
        sceneReady = solverReady = false;
        pairSource = PairSource::None;
    }

    /// Level storage regions for every scan the pipeline runs at the current capacities.
    bool layoutLevels(usize grid) {
        const auto storage = [](u32 count) { return static_cast<u32>(bk::planScan(std::max(count, 1u)).storage); };
        const u32 entryTiles = kernel::div_up(std::max(entryCap, 1u), bk::kSortTile);
        const u32 pairTiles = kernel::div_up(std::max(pairCap, 1u), bk::kSortTile);
        u32 offset = 0u;
        regionShape = offset;
        offset += storage(shapeCount);
        regionRun = offset;
        offset += storage(entryCap);
        regionPairCount = offset;
        offset += storage(entryCap);
        regionUnique = offset;
        offset += storage(pairCap);
        regionSort = offset;
        offset += std::max(storage(entryTiles * bk::kBuckets), storage(pairTiles * bk::kBuckets));
        regionNarrow = offset;
        offset += storage(static_cast<u32>(std::max<usize>(grid, 1u)));
        levelShapes = shapeCount;
        narrowGrid = grid;
        return reserve(levels, offset) &&
               reserve(sortHist, static_cast<usize>(std::max(entryTiles, pairTiles)) * bk::kBuckets);
    }

    bool ensureBroadphase(u32 entries, u32 pairSlots) {
        bool ok = true;
        if (entries > entryCap) {
            entryCap = entries;
            ok = ok && reserve(entryKeys, entryCap) && reserve(entryBodies, entryCap) &&
                 reserve(entryKeysTmp, entryCap) && reserve(entryBodiesTmp, entryCap) && reserve(runIndex, entryCap) &&
                 reserve(runStarts, static_cast<usize>(entryCap) + 1u) && reserve(uniqueCounts, entryCap) &&
                 reserve(pairOffsets, entryCap);
        }
        if (pairSlots > pairCap) {
            pairCap = pairSlots;
            ok = ok && reserve(pairKeys, pairCap) && reserve(pairKeysTmp, pairCap) && reserve(uniqueIndex, pairCap) &&
                 reserve(pairs, pairCap);
        }
        return ok && reserve(shapeCounts, shapeCount) && layoutLevels(std::max<usize>(narrowGrid, pairCap));
    }
};

// ---------------------------------------------------------------------------------------------

ResidentPhysics::ResidentPhysics() = default;

ResidentPhysics::~ResidentPhysics() { release(); }

bool ResidentPhysics::cudaAvailable() {
#if defined(FUSE_HAS_CUDA)
    return kernel::backend_available(kernel::Backend::Cuda) && cuda_backend::deviceUsable();
#else
    return false;
#endif
}

bool ResidentPhysics::init(Space space, kernel::Backend hostBackend) {
    release();
    if (space == Space::Cuda && !cudaAvailable()) {
        return false;
    }
    if (space == Space::Host && hostBackend != kernel::Backend::CpuReference &&
        hostBackend != kernel::Backend::CpuParallel) {
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->space = space;
    impl_->hostBackend = hostBackend;
#if defined(FUSE_HAS_CUDA)
    if (space == Space::Cuda) {
        impl_->entries = cuda_backend::entries();
        if (!cuda_backend::createStream(&impl_->stream) || !cuda_backend::createEvent(&impl_->eventStart) ||
            !cuda_backend::createEvent(&impl_->eventStop)) {
            impl_.reset();
            return false;
        }
    }
#endif
    if (!impl_->reserve(impl_->counters, 4u) || !impl_->reserve(impl_->status, 1u) ||
        !impl_->fill(impl_->counters.data(), 0, 4u * sizeof(u32)) ||
        !impl_->fill(impl_->status.data(), 0, sizeof(Status))) {
        impl_.reset();
        return false;
    }
    space_ = space;
    hostBackend_ = hostBackend;
    caps_ = {};
    initialized_ = true;
    return true;
}

void ResidentPhysics::release() {
    impl_.reset();
    initialized_ = false;
    caps_ = {};
}

bool ResidentPhysics::uploadBodyState(const RigidBodySoA& bodies) {
    if (!initialized_) {
        return false;
    }
    Impl& m = *impl_;
    const u32 n = bodies.count();
    if (n > 65536u || bodies.flags.size() < n || bodies.invMasses.size() < n) {
        return false;
    }
    m.bodyCount = n;
    m.orientationCount = static_cast<u32>(std::min<usize>(bodies.orientations.size(), n));
    m.layerCount = static_cast<u32>(
        std::min({bodies.collisionLayers.size(), bodies.collisionMasks.size(), static_cast<usize>(n)}));
    bool ok = m.upload(m.positions, bodies.positions.data(), n) &&
              m.upload(m.orientations, bodies.orientations.data(), m.orientationCount) &&
              m.upload(m.flags, bodies.flags.data(), n) && m.upload(m.invMasses, bodies.invMasses.data(), n) &&
              m.upload(m.layers, bodies.collisionLayers.data(), m.layerCount) &&
              m.upload(m.masks, bodies.collisionMasks.data(), m.layerCount);
    if (ok && bodies.frictionStatic.size() >= n) {
        ok = m.upload(m.frictionStatic, bodies.frictionStatic.data(), n);
    }
    caps_.bodies = static_cast<u32>(m.positions.count());
    return ok;
}

bool ResidentPhysics::uploadScene(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    if (!initialized_) {
        return false;
    }
    Impl& m = *impl_;
    m.sceneReady = false;
    const u32 s = shapes.count();
    for (u32 i = 0; i < s; ++i) {
        if (static_cast<CollisionShapeType>(shapes.types[i]) == CollisionShapeType::ConvexHull) {
            return false; // EPA / GJK pairs stay on the CPU narrowphase
        }
    }
    if (!uploadBodyState(bodies)) {
        return false;
    }
    m.shapeCount = s;
    bool ok = m.upload(m.shapeTypes, shapes.types.data(), s) && m.upload(m.shapeParams, shapes.params.data(), s) &&
              m.upload(m.shapeScalars, shapes.scalars.data(), s) &&
              m.upload(m.shapeBodies, shapes.bodyIndices.data(), s);

    // Narrowphase body -> shape map (the CPU dispatch's lookup) and the plane bodies in shape order.
    std::vector<u32>& scratch = m.hostScratch;
    scratch.resize(m.bodyCount);
    for (u32 b = 0; b < m.bodyCount; ++b) {
        scratch[b] = narrowphase::contact_shape_for_body(shapes, b);
    }
    ok = ok && m.upload(m.bodyShape, scratch.data(), m.bodyCount) && m.sync();
    scratch.clear();
    for (u32 i = 0; i < s; ++i) {
        if (shapes.bodyIndices[i] < m.bodyCount &&
            static_cast<CollisionShapeType>(shapes.types[i]) == CollisionShapeType::Plane) {
            scratch.push_back(shapes.bodyIndices[i]);
        }
    }
    m.planeCount = static_cast<u32>(scratch.size());
    ok = ok && m.upload(m.planeBodies, scratch.data(), scratch.size()) && m.sync();
    caps_.shapes = s;
    m.sceneReady = ok;
    return ok;
}

bool ResidentPhysics::uploadPairs(const std::vector<broadphase::CandidatePair>& pairList) {
    if (!initialized_) {
        return false;
    }
    Impl& m = *impl_;
    m.packedPairs.resize(pairList.size());
    for (usize i = 0; i < pairList.size(); ++i) {
        if (pairList[i].bodyA > 0xFFFFu || pairList[i].bodyB > 0xFFFFu) {
            return false;
        }
        m.packedPairs[i] = (pairList[i].bodyA << 16u) | pairList[i].bodyB;
    }
    const u32 count = static_cast<u32>(pairList.size());
    if (count > m.pairCap) {
        m.pairCap = count;
        if (!m.reserve(m.pairKeys, count) || !m.reserve(m.pairKeysTmp, count) || !m.reserve(m.uniqueIndex, count) ||
            !m.reserve(m.pairs, count) || !m.layoutLevels(std::max<usize>(m.narrowGrid, count))) {
            return false;
        }
    }
    m.pairSource = PairSource::Uploaded;
    m.pairBits = 16u;
    m.uploadedPairCount = count;
    m.pairTotal = nullptr;
    caps_.pairs = m.pairCap;
    return m.copy(m.pairs.data(), m.packedPairs.data(), count * sizeof(u32), cuda_backend::CopyKind::HostToDevice) &&
           m.sync();
}

bool ResidentPhysics::broadphase(const broadphase::SpatialHashParams& params, bool use2D) {
    if (!initialized_ || !impl_->sceneReady) {
        return false;
    }
    Impl& m = *impl_;
    const broadphase::SpatialHashParams normalized = broadphase::normalizeSpatialHashParams(params);
    const u32 bits = bk::bodyIndexBits(m.bodyCount);
    if (bits * 2u > 32u) {
        return false;
    }
    m.pairSource = PairSource::Broadphase;
    m.pairBits = bits;
    u32* counters = m.counters.data();
    bool ok = m.fill(counters, 0, 4u * sizeof(u32));
    if (m.bodyCount < 2u || m.shapeCount < 2u) {
        // preflightBroadphase: nothing to pair. Totals point at the zeroed counter.
        m.pairTotal = counters + 1;
        ok = ok && m.fill(m.status.data(), 0, sizeof(Status));
        return ok;
    }
    if (m.entryCap == 0u || m.pairCap == 0u || m.levelShapes != m.shapeCount) {
        const u32 planeSlots = m.shapeCount * m.planeCount;
        ok = ok && m.ensureBroadphase(std::max(m.entryCap, grownCapacity(4u * m.shapeCount)),
                                      std::max(m.pairCap, grownCapacity(16u * m.shapeCount + planeSlots)));
    }
    if (!ok) {
        return false;
    }
    caps_.entries = m.entryCap;
    caps_.pairs = m.pairCap;
    const u32 entryCap = m.entryCap;
    const u32 pairCap = m.pairCap;
    u32* levels = m.levels.data();
    u32* clipped = counters;

    bk::ShapeView view{};
    view.positions = m.positions.data();
    view.orientations = m.orientations.data();
    view.orientationCount = m.orientationCount;
    view.bodyCount = m.bodyCount;
    view.shapeTypes = m.shapeTypes.data();
    view.shapeParams = m.shapeParams.data();
    view.shapeBodies = m.shapeBodies.data();
    view.shapeCount = m.shapeCount;
    view.cellSize = normalized.cellSize;
    view.tableSize = normalized.tableSize;
    view.maxSpan = normalized.maxCellSpanPerAxis;
    view.maxOccupancy = normalized.maxCellOccupancy;
    view.use2D = use2D ? 1u : 0u;

#if defined(FUSE_HAS_CUDA)
    const cuda_backend::Entries& e = m.entries;
#else
    const struct {
        kernel::DeviceEntryFn cellCount, keys, pad, runFlags, runStarts, cells, pairs, planePairs, uniqueFlags,
            uniqueWrite, status;
    } e{};
#endif

    // 1. Shape -> cells: count, scan, (key, body) entries (clipped at the entry capacity).
    ok = ok && m.launch<bk::CellCountKernel>(kBpCountName, m.shapeCount, e.cellCount,
                                             bk::CellCountParams{view, m.shapeCounts.data()});
    const u32* entryTotal = m.scan(m.shapeCounts.data(), m.shapeCount, levels + m.regionShape, ok);
    ok = ok && m.launch<KeysKernel>(kBpKeysName, m.shapeCount, e.keys,
                                    KeysParams{view, m.shapeCounts.data(), m.entryKeys.data(), m.entryBodies.data(),
                                               entryCap, clipped});

    // 2. Stable radix sort of the entries by cell key over the whole capacity (sentinel padding).
    const u32 keyBits = bk::bodyIndexBits(normalized.tableSize);
    ok = ok && m.launch<PadKernel>(kBpPadName, entryCap, e.pad,
                                   PadParams{m.entryKeys.data(), m.entryBodies.data(), entryTotal, 0u, entryCap,
                                             sortSentinel(keyBits), 0xFFFFFFFFu});
    bool inTemp = false;
    ok = ok && bk::launchRadixSort(m.backend(), m.entryKeys.data(), m.entryBodies.data(), m.entryKeysTmp.data(),
                                   m.entryBodiesTmp.data(), m.sortHist.data(), levels + m.regionSort, entryCap,
                                   keyBits, m.sortEntries(), &inTemp);
    u32* sortedKeys = inTemp ? m.entryKeysTmp.data() : m.entryKeys.data();
    u32* sortedBodies = inTemp ? m.entryBodiesTmp.data() : m.entryBodies.data();

    // 3. Cells = runs of equal keys.
    ok = ok && m.launch<RunFlagKernel>(kBpRunsName, entryCap, e.runFlags,
                                       RunFlagParams{sortedKeys, entryTotal, entryCap, m.runIndex.data()});
    const u32* runTotal = m.scan(m.runIndex.data(), entryCap, levels + m.regionRun, ok);
    ok = ok && m.launch<RunStartKernel>(kBpRunStartsName, entryCap, e.runStarts,
                                        RunStartParams{sortedKeys, entryTotal, m.runIndex.data(), runTotal, entryCap,
                                                       m.runStarts.data()});

    // 4. Per cell: sort + unique occupants, pair counts, scan, packed canonical pair keys.
    ok = ok && m.launch<CellsKernel>(kBpCellsName, entryCap, e.cells,
                                     CellsParams{m.runStarts.data(), runTotal, entryCap, sortedBodies,
                                                 m.uniqueCounts.data(), m.pairOffsets.data()});
    const u32* cellPairTotal = m.scan(m.pairOffsets.data(), entryCap, levels + m.regionPairCount, ok);
    PairsParams pairParams{};
    pairParams.starts = m.runStarts.data();
    pairParams.runTotal = runTotal;
    pairParams.capacity = entryCap;
    pairParams.bodies = sortedBodies;
    pairParams.uniqueCounts = m.uniqueCounts.data();
    pairParams.pairOffsets = m.pairOffsets.data();
    pairParams.bits = bits;
    pairParams.pairKeys = m.pairKeys.data();
    pairParams.pairCapacity = pairCap;
    pairParams.clipped = clipped;
    ok = ok && m.launch<PairsKernel>(kBpPairsName, entryCap, e.pairs, pairParams);

    // 5. Plane pairs for every dynamic shape (the plane merge), appended after the cell pairs.
    const u32 pairKeyBits = bits * 2u;
    const u32 pairSentinel = sortSentinel(pairKeyBits);
    const u32 planeSlots = m.shapeCount * m.planeCount;
    if (m.planeCount > 0u) {
        PlanePairsParams plane{};
        plane.shapeTypes = m.shapeTypes.data();
        plane.shapeBodies = m.shapeBodies.data();
        plane.shapeCount = m.shapeCount;
        plane.flags = m.flags.data();
        plane.layers = m.layerCount > 0u ? m.layers.data() : nullptr;
        plane.masks = m.layerCount > 0u ? m.masks.data() : nullptr;
        plane.layerCount = m.layerCount;
        plane.bodyCount = m.bodyCount;
        plane.planeBodies = m.planeBodies.data();
        plane.planeCount = m.planeCount;
        plane.cellPairTotal = cellPairTotal;
        plane.bits = bits;
        plane.sentinel = pairSentinel;
        plane.pairKeys = m.pairKeys.data();
        plane.pairCapacity = pairCap;
        plane.clipped = clipped;
        ok = ok && m.launch<PlanePairsKernel>(kBpPlanePairsName, m.shapeCount, e.planePairs, plane);
    }

    // 6. Dedupe: sort every slot (sentinel padding), flag the first of each run, scan, write.
    ok = ok && m.launch<PadKernel>(kBpPadName, pairCap, e.pad,
                                   PadParams{m.pairKeys.data(), nullptr, cellPairTotal, planeSlots, pairCap,
                                             pairSentinel, 0u});
    ok = ok && bk::launchRadixSort(m.backend(), m.pairKeys.data(), nullptr, m.pairKeysTmp.data(), nullptr,
                                   m.sortHist.data(), levels + m.regionSort, pairCap, pairKeyBits, m.sortEntries(),
                                   &inTemp);
    const u32* sortedPairs = inTemp ? m.pairKeysTmp.data() : m.pairKeys.data();
    ok = ok && m.launch<UniqueFlagKernel>(kBpUniqueFlagsName, pairCap, e.uniqueFlags,
                                          UniqueFlagParams{sortedPairs, pairSentinel, m.uniqueIndex.data()});
    m.pairTotal = m.scan(m.uniqueIndex.data(), pairCap, levels + m.regionUnique, ok);
    ok = ok && m.launch<UniqueWriteKernel>(kBpUniqueWriteName, pairCap, e.uniqueWrite,
                                           UniqueWriteParams{sortedPairs, m.uniqueIndex.data(), pairSentinel,
                                                             m.pairs.data()});

    StatusParams statusParams{};
    statusParams.status = m.status.data();
    statusParams.entryTotal = entryTotal;
    statusParams.entryCapacity = entryCap;
    statusParams.runTotal = runTotal;
    statusParams.cellPairTotal = cellPairTotal;
    statusParams.planeSlots = planeSlots;
    statusParams.pairCapacity = pairCap;
    statusParams.pairTotal = m.pairTotal;
    statusParams.clipped = clipped;
    ok = ok && m.launch<StatusKernel>(kStatusName, 1u, e.status, statusParams, kernel::Dim3{1u, 1u, 1u});
    return ok;
}

bool ResidentPhysics::narrowphase(f32 margin, bool speculative) {
    if (!initialized_ || !impl_->sceneReady || impl_->pairSource == PairSource::None) {
        return false;
    }
    Impl& m = *impl_;
    const bool uploaded = m.pairSource == PairSource::Uploaded;
    const u32 grid = uploaded ? m.uploadedPairCount : m.pairCap;
    bool ok = true;
    if (grid > m.staging.count() || grid > m.npFlags.count()) {
        ok = m.reserve(m.staging, grid) && m.reserve(m.npFlags, grid) && m.reserve(m.npOffsets, grid);
    }
    if (ok && (m.narrowGrid < grid || m.levelShapes != m.shapeCount)) {
        // Relaying out moves the scan regions (and may reallocate them): only safe when no device total
        // of a queued broadphase is still to be read. broadphase() plans the narrowphase region itself.
        ok = uploaded && m.layoutLevels(grid);
    }
    if (ok && m.contactCap == 0u) {
        m.contactCap = grownCapacity(std::min<u32>(grid, std::max<u32>(grid / 4u, 1024u)));
        ok = m.reserve(m.contacts, m.contactCap);
    }
    if (!ok) {
        return false;
    }
    caps_.contacts = m.contactCap;

#if defined(FUSE_HAS_CUDA)
    const cuda_backend::Entries& e = m.entries;
#else
    const struct {
        kernel::DeviceEntryFn detect, compact, status;
    } e{};
#endif

    DetectParams detect{};
    detect.bodies = NarrowBodies{m.positions.data(), m.orientations.data(), m.orientationCount, m.flags.data(),
                                 m.invMasses.data(), m.bodyCount};
    detect.shapes = NarrowShapes{m.shapeTypes.data(), m.shapeParams.data(), m.shapeScalars.data(), m.shapeCount,
                                 m.bodyShape.data()};
    detect.pairKeys = m.pairs.data();
    detect.pairTotal = uploaded ? nullptr : m.pairTotal;
    detect.pairCount = m.uploadedPairCount;
    detect.capacity = grid;
    detect.bits = m.pairBits;
    detect.margin = margin;
    detect.speculative = speculative ? 1u : 0u;
    detect.staging = m.staging.data();
    detect.flags = m.npFlags.data();
    detect.offsets = m.npOffsets.data();
    ok = m.launch<DetectKernel>(kNpDetectName, grid, e.detect, detect);
    const u32* contactTotal = m.counters.data() + 1; // zero when there is nothing to scan
    if (grid > 0u) {
        contactTotal = m.scan(m.npOffsets.data(), grid, m.levels.data() + m.regionNarrow, ok);
    }
    ok = ok && m.launch<CompactKernel>(kNpCompactName, grid, e.compact,
                                       CompactParams{m.staging.data(), m.npFlags.data(), m.npOffsets.data(),
                                                     m.contacts.data(), m.contactCap});
    StatusParams statusParams{};
    statusParams.status = m.status.data();
    statusParams.contactTotal = contactTotal;
    statusParams.contactCapacity = m.contactCap;
    ok = ok && m.launch<StatusKernel>(kStatusName, 1u, e.status, statusParams, kernel::Dim3{1u, 1u, 1u});
    return ok;
}

bool ResidentPhysics::growFromStatus(const Status& s) {
    if (!initialized_ || s.overflow == 0u) {
        return false;
    }
    Impl& m = *impl_;
    bool ok = true;
    if ((s.overflow & (kOverflowEntries | kOverflowPairs)) != 0u) {
        const u32 entries = (s.overflow & kOverflowEntries) != 0u ? grownCapacity(s.entries) : m.entryCap;
        // An entry overflow truncates the cells, so the pair count is a lower bound: grow both.
        const u32 slots = (s.overflow & kOverflowPairs) != 0u || (s.overflow & kOverflowEntries) != 0u
                              ? std::max(m.pairCap, grownCapacity(s.pairSlots))
                              : m.pairCap;
        ok = m.ensureBroadphase(std::max(entries, m.entryCap), slots);
    }
    if ((s.overflow & kOverflowContacts) != 0u) {
        m.contactCap = grownCapacity(s.contacts);
        ok = ok && m.reserve(m.contacts, m.contactCap);
    }
    caps_.entries = m.entryCap;
    caps_.pairs = m.pairCap;
    caps_.contacts = m.contactCap;
    return ok;
}

bool ResidentPhysics::fitBroadphase(const broadphase::SpatialHashParams& params, bool use2D, Status* out) {
    Status s{};
    for (u32 attempt = 0; attempt < 8u; ++attempt) {
        if (!broadphase(params, use2D) || !readStatus(s)) {
            return false;
        }
        if (s.overflow == 0u) {
            if (out != nullptr) {
                *out = s;
            }
            return true;
        }
        if (!growFromStatus(s)) {
            return false;
        }
    }
    return false;
}

bool ResidentPhysics::fitNarrowphase(f32 margin, bool speculative, Status* out) {
    Status s{};
    for (u32 attempt = 0; attempt < 8u; ++attempt) {
        if (!narrowphase(margin, speculative) || !readStatus(s)) {
            return false;
        }
        if ((s.overflow & kOverflowContacts) == 0u) {
            if (out != nullptr) {
                *out = s;
            }
            return true;
        }
        if (!growFromStatus(s)) {
            return false;
        }
    }
    return false;
}

bool ResidentPhysics::uploadSolver(const SolverProblem& problem) {
    if (!initialized_ || problem.bodies == nullptr || problem.contacts == nullptr || problem.coloring == nullptr) {
        return false;
    }
    Impl& m = *impl_;
    m.solverReady = false;
    const RigidBodySoA& bodies = *problem.bodies;
    const u32 n = bodies.count();
    if (bodies.predictedPositions.size() < n || bodies.predictedOrientations.size() < n || bodies.flags.size() < n ||
        bodies.invMasses.size() < n || bodies.frictionStatic.size() < n) {
        return false;
    }
    const ConstraintColoring& coloring = *problem.coloring;
    const auto sizeOf = [](const auto* v) { return v != nullptr ? static_cast<u32>(v->size()) : 0u; };
    m.solverBodies = n;
    m.solverContacts = static_cast<u32>(problem.contacts->size());
    m.solverDistances = sizeOf(problem.distances);
    m.invInertiaCount = sizeOf(problem.invInertia);
    m.anchorCount = sizeOf(problem.anchors);
    m.contactLambdaCount = sizeOf(problem.contactLambdas);
    m.pointLambdaCount = sizeOf(problem.pointLambdas);
    m.distanceLambdaCount = sizeOf(problem.distanceLambdas);
    if (m.contactLambdaCount < m.solverContacts || m.distanceLambdaCount < m.solverDistances) {
        return false; // PBDSolver sizes them (ensureLambdaCapacity) before the iterations
    }
    const auto dataOf = [](const auto* v) { return v != nullptr ? v->data() : nullptr; };
    bool ok = m.upload(m.predictedPositions, bodies.predictedPositions.data(), n) &&
              m.upload(m.predictedOrientations, bodies.predictedOrientations.data(), n) &&
              m.upload(m.solverFlags, bodies.flags.data(), n) &&
              m.upload(m.solverInvMasses, bodies.invMasses.data(), n) &&
              m.upload(m.solverFriction, bodies.frictionStatic.data(), n) &&
              m.upload(m.invInertia, dataOf(problem.invInertia), m.invInertiaCount) &&
              m.upload(m.solverContactBuffer, problem.contacts->data(), m.solverContacts) &&
              m.upload(m.anchors, dataOf(problem.anchors), m.anchorCount) &&
              m.upload(m.contactLambdas, dataOf(problem.contactLambdas), m.contactLambdaCount) &&
              m.upload(m.pointLambdas, dataOf(problem.pointLambdas), m.pointLambdaCount) &&
              m.upload(m.distances, dataOf(problem.distances), m.solverDistances) &&
              m.upload(m.distanceLambdas, dataOf(problem.distanceLambdas), m.distanceLambdaCount) &&
              m.upload(m.colorItems, coloring.items.data(), coloring.items.size());
    std::copy(coloring.colorStart, coloring.colorStart + ConstraintColoring::kMaxColors + 2u, m.colorStart);
    m.colorCount = coloring.colorCount;
    m.overflowCount = coloring.overflowCount;
    m.solverReady = ok;
    return ok;
}

bool ResidentPhysics::solveIterations(u32 iterations, f32 dt, f32 contactCompliance) {
    if (!initialized_ || !impl_->solverReady) {
        return false;
    }
    Impl& m = *impl_;
#if defined(FUSE_HAS_CUDA)
    const kernel::DeviceEntryFn colorEntry = m.entries.solveColor;
    const kernel::DeviceEntryFn serialEntry = m.entries.solveSerial;
#else
    const kernel::DeviceEntryFn colorEntry = nullptr;
    const kernel::DeviceEntryFn serialEntry = nullptr;
#endif
    SolveParams p{};
    p.bodies = SolverBodies{m.predictedPositions.data(), m.predictedOrientations.data(), m.solverFlags.data(),
                            m.solverInvMasses.data(), m.solverFriction.data(), m.invInertia.data(), m.invInertiaCount};
    p.constraints = SolverConstraints{m.solverContactBuffer.data(), m.anchors.data(), m.anchorCount,
                                      m.contactLambdas.data(), m.pointLambdas.data(), m.pointLambdaCount,
                                      m.distances.data(), m.distanceLambdas.data()};
    p.dt = dt;
    p.compliance = contactCompliance;
    bool ok = true;
    for (u32 iter = 0; iter < iterations && ok; ++iter) {
        for (u32 color = 0; color < m.colorCount && ok; ++color) {
            p.items = m.colorItems.data() + m.colorStart[color];
            ok = m.launch<SolveColorKernel>(kSolveColorName, m.colorStart[color + 1u] - m.colorStart[color],
                                            colorEntry, p);
        }
        if (m.overflowCount > 0u && ok) {
            p.items = m.colorItems.data() + m.colorStart[ConstraintColoring::kMaxColors];
            p.count = m.overflowCount;
            ok = m.launch<SolveSerialKernel>(kSolveSerialName, 1u, serialEntry, p, kernel::Dim3{1u, 1u, 1u});
            p.count = 0u;
        }
    }
    return ok;
}

bool ResidentPhysics::downloadSolver(RigidBodySoA& bodies, std::vector<f32>* contactLambdas,
                                     std::vector<f32>* pointLambdas, std::vector<f32>* distanceLambdas) {
    if (!initialized_ || !impl_->solverReady) {
        return false;
    }
    Impl& m = *impl_;
    const u32 n = m.solverBodies;
    bodies.predictedPositions.resize(std::max<usize>(bodies.predictedPositions.size(), n));
    bodies.predictedOrientations.resize(std::max<usize>(bodies.predictedOrientations.size(), n));
    bool ok = m.download(bodies.predictedPositions.data(), m.predictedPositions.data(), n) &&
              m.download(bodies.predictedOrientations.data(), m.predictedOrientations.data(), n);
    if (contactLambdas != nullptr) {
        contactLambdas->resize(m.contactLambdaCount);
        ok = ok && m.download(contactLambdas->data(), m.contactLambdas.data(), m.contactLambdaCount);
    }
    if (pointLambdas != nullptr) {
        pointLambdas->resize(m.pointLambdaCount);
        ok = ok && m.download(pointLambdas->data(), m.pointLambdas.data(), m.pointLambdaCount);
    }
    if (distanceLambdas != nullptr) {
        distanceLambdas->resize(m.distanceLambdaCount);
        ok = ok && m.download(distanceLambdas->data(), m.distanceLambdas.data(), m.distanceLambdaCount);
    }
    return ok && m.sync();
}

bool ResidentPhysics::synchronize() { return initialized_ && impl_->sync(); }

bool ResidentPhysics::readStatus(Status& out) {
    if (!initialized_) {
        return false;
    }
    Impl& m = *impl_;
    return m.download(&out, m.status.data(), 1u) && m.sync();
}

bool ResidentPhysics::downloadPairs(broadphase::PairBufferSoA& out) {
    out.clear();
    Status s{};
    if (!initialized_ || impl_->pairSource != PairSource::Broadphase || !readStatus(s)) {
        return false;
    }
    Impl& m = *impl_;
    const u32 count = std::min(s.pairs, m.pairCap);
    std::vector<u32>& keys = m.hostScratch;
    keys.resize(count);
    if (!m.download(keys.data(), m.pairs.data(), count) || !m.sync()) {
        return false;
    }
    if (count > out.bodyA.capacity()) {
        out.reserve(count);
    }
    const u32 lowMask = (1u << m.pairBits) - 1u;
    for (u32 i = 0; i < count; ++i) {
        out.push(keys[i] >> m.pairBits, keys[i] & lowMask);
    }
    return s.overflow == 0u;
}

bool ResidentPhysics::downloadContacts(std::vector<narrowphase::ContactManifold>& out) {
    out.clear();
    Status s{};
    if (!initialized_ || !readStatus(s)) {
        return false;
    }
    Impl& m = *impl_;
    const u32 count = std::min(s.contacts, m.contactCap);
    out.resize(count);
    return m.download(out.data(), m.contacts.data(), count) && m.sync() && (s.overflow & kOverflowContacts) == 0u;
}

bool ResidentPhysics::timerBegin() {
    if (!initialized_) {
        return false;
    }
    Impl& m = *impl_;
    if (m.space == Space::Host) {
        m.hostStart = std::chrono::steady_clock::now();
        return true;
    }
#if defined(FUSE_HAS_CUDA)
    return cuda_backend::recordEvent(m.eventStart, m.stream);
#else
    return false;
#endif
}

double ResidentPhysics::timerEndMs() {
    if (!initialized_) {
        return -1.0;
    }
    Impl& m = *impl_;
    if (m.space == Space::Host) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m.hostStart).count();
    }
#if defined(FUSE_HAS_CUDA)
    float ms = 0.f;
    if (!cuda_backend::recordEvent(m.eventStop, m.stream) || !cuda_backend::elapsedMs(m.eventStart, m.eventStop, &ms)) {
        return -1.0;
    }
    return static_cast<double>(ms);
#else
    return -1.0;
#endif
}

} // namespace fuse::physics::resident
