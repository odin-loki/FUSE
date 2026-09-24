#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rg/sync_model.hpp>

#include <algorithm>
#include <chrono>

namespace fuse::renderer::rg {

namespace {

u8 queueIndex(QueueClass queue) {
    return static_cast<u8>(queue);
}

bool visible(const u64 (&stages)[2], const u64 (&access)[2], u64 s, u64 a) {
    for (u32 i = 0; i < 2; ++i) {
        if (stages[i] != 0u && (s & ~stages[i]) == 0u && (a & ~access[i]) == 0u) {
            return true;
        }
    }
    return false;
}

void addVisible(u64 (&stages)[2], u64 (&access)[2], u64 s, u64 a) {
    if (stages[0] == s) {
        access[0] |= a;
        return;
    }
    stages[1] = stages[0];
    access[1] = access[0];
    stages[0] = s;
    access[0] = a;
}

void clearVisible(u64 (&stages)[2], u64 (&access)[2]) {
    stages[0] = stages[1] = 0;
    access[0] = access[1] = 0;
}

} // namespace

// --- PassContext / PassBuilder ------------------------------------------------------------------

void* PassContext::image(TextureRef ref) const {
    if (graph == nullptr || ref.id == 0u || ref.id > graph->m_resources.size()) {
        return nullptr;
    }
    const auto& resource = graph->m_resources[ref.id - 1u];
    return resource.image ? resource.handle : nullptr;
}

void* PassContext::imageView(TextureRef ref) const {
    if (graph == nullptr || ref.id == 0u || ref.id > graph->m_resources.size()) {
        return nullptr;
    }
    const auto& resource = graph->m_resources[ref.id - 1u];
    return resource.image ? resource.view : nullptr;
}

void* PassContext::buffer(BufferRef ref) const {
    if (graph == nullptr || ref.id == 0u || ref.id > graph->m_resources.size()) {
        return nullptr;
    }
    const auto& resource = graph->m_resources[ref.id - 1u];
    return resource.image ? nullptr : resource.handle;
}

PassBuilder& PassBuilder::use(TextureRef texture, Access access, ImageRange range, u8 shaderStages) {
    Graph::Resource* resource = m_graph->resourceFor(texture);
    if (resource == nullptr) {
        ++m_graph->m_invalidRefs;
        return *this;
    }
    Graph::Use use;
    use.resource = texture.id - 1u;
    use.access = access;
    use.shaderStages = shaderStages;
    use.image = true;
    use.imageRange = range;
    m_graph->addUse(m_pass, use);
    return *this;
}

PassBuilder& PassBuilder::use(BufferRef buffer, Access access, BufferRange range, u8 shaderStages) {
    Graph::Resource* resource = m_graph->resourceFor(buffer);
    if (resource == nullptr) {
        ++m_graph->m_invalidRefs;
        return *this;
    }
    Graph::Use use;
    use.resource = buffer.id - 1u;
    use.access = access;
    use.shaderStages = shaderStages;
    use.image = false;
    use.bufferRange = range;
    m_graph->addUse(m_pass, use);
    return *this;
}

PassBuilder& PassBuilder::neverCull() {
    if (m_pass < m_graph->m_passes.size()) {
        m_graph->m_passes[m_pass].neverCull = true;
    }
    return *this;
}

// --- Graph: declaration -------------------------------------------------------------------------

bool Graph::Pending::operator==(const Pending& o) const {
    return srcStages == o.srcStages && srcAccess == o.srcAccess && dstStages == o.dstStages &&
           dstAccess == o.dstAccess && oldLayout == o.oldLayout && newLayout == o.newLayout &&
           srcQueue == o.srcQueue && dstQueue == o.dstQueue && releaseBatch == o.releaseBatch;
}

void Graph::reset() {
    m_stats = {};
    m_passes.clear();
    m_uses.clear();
    m_resources.clear();
    m_order.clear();
    m_batches.clear();
    m_imageBarriers.clear();
    m_lateImageBarriers.clear();
    m_bufferBarriers.clear();
    m_post.clear();
    m_states.clear();
    m_invalidRefs = 0;
}

Graph::Resource* Graph::resourceFor(TextureRef ref) {
    if (ref.id == 0u || ref.id > m_resources.size() || !m_resources[ref.id - 1u].image) {
        return nullptr;
    }
    return &m_resources[ref.id - 1u];
}

Graph::Resource* Graph::resourceFor(BufferRef ref) {
    if (ref.id == 0u || ref.id > m_resources.size() || m_resources[ref.id - 1u].image) {
        return nullptr;
    }
    return &m_resources[ref.id - 1u];
}

TextureRef Graph::importImage(const ImportedImage& image) {
    Resource resource;
    resource.image = true;
    resource.imported = true;
    resource.handle = image.image;
    resource.view = image.view;
    resource.format = image.format;
    resource.width = std::max(image.width, 1u);
    resource.height = std::max(image.height, 1u);
    resource.depth = std::max(image.depth, 1u);
    resource.mips = std::max(image.mipLevels, 1u);
    resource.layers = std::max(image.arrayLayers, 1u);
    resource.initialLayout = image.initialLayout;
    resource.finalLayout = image.finalLayout;
    resource.layoutTracker = image.layoutTracker;
    resource.queueTracker = image.queueTracker;
    resource.initialQueue = image.initialQueue;
    resource.name = image.name;
    m_resources.push_back(resource);
    return {static_cast<u32>(m_resources.size())};
}

BufferRef Graph::importBuffer(const ImportedBuffer& buffer) {
    Resource resource;
    resource.image = false;
    resource.imported = true;
    resource.handle = buffer.buffer;
    resource.size = buffer.size;
    resource.initialQueue = buffer.initialQueue;
    resource.queueTracker = buffer.queueTracker;
    resource.name = buffer.name;
    m_resources.push_back(resource);
    return {static_cast<u32>(m_resources.size())};
}

TextureRef Graph::createImage(const ImageDesc& desc) {
    Resource resource;
    resource.image = true;
    resource.imported = false;
    resource.format = desc.format;
    resource.width = std::max(desc.width, 1u);
    resource.height = std::max(desc.height, 1u);
    resource.depth = std::max(desc.depth, 1u);
    resource.mips = std::max(desc.mipLevels, 1u);
    resource.layers = std::max(desc.arrayLayers, 1u);
    resource.extraUsage = desc.extraUsage;
    resource.name = desc.name;
    m_resources.push_back(resource);
    return {static_cast<u32>(m_resources.size())};
}

BufferRef Graph::createBuffer(const BufferDesc& desc) {
    Resource resource;
    resource.image = false;
    resource.imported = false;
    resource.size = desc.size;
    resource.extraUsage = desc.extraUsage;
    resource.name = desc.name;
    m_resources.push_back(resource);
    return {static_cast<u32>(m_resources.size())};
}

PassBuilder Graph::addPass(const char* name, PassFn fn, void* user, QueueClass queue) {
    Pass pass;
    pass.name = name != nullptr ? name : "pass";
    pass.fn = fn;
    pass.user = user;
    pass.requested = queue;
    pass.queue = queue;
    pass.useBegin = static_cast<u32>(m_uses.size());
    m_passes.push_back(pass);
    return PassBuilder(this, static_cast<u32>(m_passes.size() - 1u));
}

u32 Graph::addUse(u32 pass, const Use& use) {
    // Uses live contiguously per pass: only the newest pass can take more.
    if (pass + 1u != m_passes.size()) {
        ++m_invalidRefs;
        return UINT32_MAX;
    }
    m_uses.push_back(use);
    ++m_passes[pass].useCount;
    return static_cast<u32>(m_uses.size() - 1u);
}

Lifetime Graph::lifetime(TextureRef ref) const {
    if (ref.id == 0u || ref.id > m_resources.size() || !m_resources[ref.id - 1u].image) {
        return {};
    }
    return m_resources[ref.id - 1u].life;
}

Lifetime Graph::lifetime(BufferRef ref) const {
    if (ref.id == 0u || ref.id > m_resources.size() || m_resources[ref.id - 1u].image) {
        return {};
    }
    return m_resources[ref.id - 1u].life;
}

// --- Graph: compile -----------------------------------------------------------------------------

bool Graph::compile(const CompileOptions& options) {
    const auto start = std::chrono::steady_clock::now();
    m_options = options;
    m_stats = {};
    m_stats.passCount = static_cast<u32>(m_passes.size());
    m_order.clear();
    m_batches.clear();
    m_imageBarriers.clear();
    m_lateImageBarriers.clear();
    m_bufferBarriers.clear();
    m_post.clear();
    if (m_passes.empty()) {
        return false;
    }

    auto resolve = [&](QueueClass queue) {
        if (options.forceQueue != kNoQueue) {
            return static_cast<QueueClass>(options.forceQueue);
        }
        if (queue == QueueClass::AsyncCompute && !options.asyncComputeAvailable) {
            return QueueClass::Graphics;
        }
        if (queue == QueueClass::Transfer && !options.transferAvailable) {
            return QueueClass::Graphics;
        }
        return queue;
    };

    // Queue resolution: unavailable classes and accesses the queue cannot perform fall back.
    for (Pass& pass : m_passes) {
        pass.queue = resolve(pass.requested);
        pass.culled = false;
        pass.pre = {};
        pass.late = {};
        if (options.forceQueue != kNoQueue) {
            pass.queue = static_cast<QueueClass>(options.forceQueue);
            continue;
        }
        if (pass.queue == QueueClass::Graphics) {
            continue;
        }
        bool ok = true;
        for (u32 u = pass.useBegin; u < pass.useBegin + pass.useCount; ++u) {
            const AccessInfo info = describeAccess(m_uses[u].access, m_uses[u].shaderStages, pass.queue);
            if (info.graphicsOnly || (pass.queue == QueueClass::Transfer && !info.transferOk)) {
                ok = false;
            }
        }
        if (!ok) {
            pass.queue = QueueClass::Graphics;
            ++m_stats.queueFallbacks;
        }
    }

    // Culling, back to front: a pass is live if it has side effects (writes an imported resource,
    // presents, declares a host readback, neverCull) or writes something a live later pass uses.
    std::vector<u8>& needed = m_scratchNeeded;
    needed.assign(m_resources.size(), 0u);
    for (u32 p = static_cast<u32>(m_passes.size()); p-- > 0u;) {
        Pass& pass = m_passes[p];
        bool live = pass.neverCull;
        for (u32 u = pass.useBegin; u < pass.useBegin + pass.useCount && !live; ++u) {
            const Use& use = m_uses[u];
            const AccessInfo info = describeAccess(use.access, use.shaderStages, pass.queue);
            if ((info.write && m_resources[use.resource].imported) || use.access == Access::Present ||
                use.access == Access::HostRead || (info.write && needed[use.resource] != 0u)) {
                live = true;
            }
        }
        pass.culled = !live;
        if (live) {
            for (u32 u = pass.useBegin; u < pass.useBegin + pass.useCount; ++u) {
                needed[m_uses[u].resource] = 1u;
            }
        }
    }

    for (u32 p = 0; p < m_passes.size(); ++p) {
        if (!m_passes[p].culled) {
            m_order.push_back(p);
        }
    }
    m_stats.executedPasses = static_cast<u32>(m_order.size());
    m_stats.culledPasses = m_stats.passCount - m_stats.executedPasses;

    // Lifetimes and derived usage.
    for (Resource& resource : m_resources) {
        resource.life = {};
        resource.usage = resource.extraUsage;
        resource.aliasSlot = UINT32_MAX;
        resource.aliasPrev = UINT32_MAX;
    }
    for (u32 pos = 0; pos < m_order.size(); ++pos) {
        const Pass& pass = m_passes[m_order[pos]];
        for (u32 u = pass.useBegin; u < pass.useBegin + pass.useCount; ++u) {
            const Use& use = m_uses[u];
            Resource& resource = m_resources[use.resource];
            const AccessInfo info = describeAccess(use.access, use.shaderStages, pass.queue);
            resource.usage |= resource.image ? info.imageUsage : info.bufferUsage;
            if (resource.life.first == UINT32_MAX) {
                resource.life.first = pos;
            }
            resource.life.last = pos;
        }
    }
    for (const Resource& resource : m_resources) {
        if (!resource.imported && resource.life.first != UINT32_MAX) {
            if (resource.image) {
                ++m_stats.transientImages;
            } else {
                ++m_stats.transientBuffers;
            }
        }
    }

    // Prologue batches: an imported resource owned by queue Q at graph start whose first use is on
    // another queue needs its ownership released on Q before anything else.
    m_scratchPrologue.assign(kQueueClassCount, -1);
    for (const Resource& resource : m_resources) {
        if (!resource.imported || resource.initialQueue == kNoQueue || resource.life.first == UINT32_MAX ||
            (resource.image && resource.initialLayout == vkc::kLayoutUndefined)) {
            continue;
        }
        const QueueClass owner = resolve(static_cast<QueueClass>(resource.initialQueue));
        const QueueClass firstQueue = m_passes[m_order[resource.life.first]].queue;
        if (owner != firstQueue && m_scratchPrologue[queueIndex(owner)] < 0) {
            Batch batch;
            batch.queue = owner;
            batch.orderBegin = 0;
            batch.orderCount = 0;
            batch.prologue = true;
            m_scratchPrologue[queueIndex(owner)] = static_cast<i32>(m_batches.size());
            m_batches.push_back(batch);
        }
    }

    // Submission batches: maximal runs of consecutive passes on one queue.
    for (u32 pos = 0; pos < m_order.size(); ++pos) {
        Pass& pass = m_passes[m_order[pos]];
        const bool prologueTail = !m_batches.empty() && m_batches.back().prologue;
        if (m_batches.empty() || prologueTail || m_batches.back().queue != pass.queue) {
            Batch batch;
            batch.queue = pass.queue;
            batch.orderBegin = pos;
            m_batches.push_back(batch);
        }
        ++m_batches.back().orderCount;
        pass.batch = static_cast<u32>(m_batches.size() - 1u);
    }
    m_stats.batchCount = static_cast<u32>(m_batches.size());
    m_stats.compiled = !m_order.empty();

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    m_stats.compileUs = elapsed > 0 ? static_cast<u32>(elapsed) : 0u;
    return m_stats.compiled;
}

// --- Graph: barrier planning --------------------------------------------------------------------

void Graph::resetStates() {
    u32 total = 0;
    for (Resource& resource : m_resources) {
        resource.stateBase = total;
        resource.stateCount = resource.image ? resource.mips * resource.layers : 1u;
        total += resource.stateCount;
    }
    m_states.assign(total, SubState{});
    for (const Resource& resource : m_resources) {
        for (u32 i = 0; i < resource.stateCount; ++i) {
            SubState& state = m_states[resource.stateBase + i];
            // Prior (external / previous frame / previous alias occupant) accesses are unknown:
            // the first barrier waits on everything earlier on the queue.
            state.writeStages = vkc::kStageAllCommands;
            state.writeAccess = vkc::kAccessMemoryWrite;
            if (resource.imported) {
                state.layout = resource.image ? resource.initialLayout : 0u;
                state.contents = !resource.image || resource.initialLayout != vkc::kLayoutUndefined;
                if (resource.initialQueue != kNoQueue) {
                    QueueClass owner = static_cast<QueueClass>(resource.initialQueue);
                    if (m_options.forceQueue != kNoQueue) {
                        owner = static_cast<QueueClass>(m_options.forceQueue);
                    }
                    if (owner == QueueClass::AsyncCompute && !m_options.asyncComputeAvailable &&
                        m_options.forceQueue == kNoQueue) {
                        owner = QueueClass::Graphics;
                    }
                    if (owner == QueueClass::Transfer && !m_options.transferAvailable &&
                        m_options.forceQueue == kNoQueue) {
                        owner = QueueClass::Graphics;
                    }
                    state.queue = queueIndex(owner);
                    state.lastBatch[state.queue] = m_scratchPrologue[state.queue];
                }
            }
        }
    }
}

bool Graph::computePending(const Resource& resource, SubState& s, const Use& use, u32 orderPos, u32 batchIndex,
                           u64 S, u64 A, u32 L, bool W, Pending& out) {
    (void)use;
    const u8 q = queueIndex(m_batches[batchIndex].queue);
    Batch& batch = m_batches[batchIndex];
    out = Pending{};
    bool need = false;

    auto waitOn = [&](u8 otherQueue, i32 otherBatch) {
        if (otherQueue == q || otherBatch < 0) {
            return;
        }
        if (batch.waitBatch[otherQueue] < otherBatch) {
            if (batch.waitBatch[otherQueue] < 0) {
                ++m_stats.crossQueueWaits;
            }
            batch.waitBatch[otherQueue] = otherBatch;
        }
    };

    // Same subresource used twice in one pass (overlapping, differently declared ranges): widen
    // the state; if the new access is not covered, a conservative full barrier keeps it correct.
    if (s.touched && s.lastPass == orderPos) {
        if (resource.image && L != s.layout) {
            ++m_stats.layoutConflicts;
        }
        const bool covered = visible(s.visStages, s.visAccess, S, A) || (s.writeStages & S) == S;
        if (!covered) {
            out.srcStages = vkc::kStageAllCommands;
            out.srcAccess = vkc::kAccessMemoryWrite;
            out.dstStages = S;
            out.dstAccess = A;
            out.oldLayout = s.layout;
            out.newLayout = s.layout;
            need = true;
        }
        if (W) {
            s.writeStages |= S;
            s.writeAccess |= A & vkc::kAccessWriteMask;
            s.contents = true;
        } else {
            s.readStages |= S;
            addVisible(s.visStages, s.visAccess, S, A);
        }
        return need;
    }

    // First touch of a transient that reuses the memory of an earlier (dead) transient. The
    // occupant may have covered a different byte range than this resource (and the memory held
    // last frame's data before that), so the first barrier keeps the conservative
    // ALL_COMMANDS / MEMORY_WRITE source from resetStates(); an occupant on another queue adds a
    // timeline wait on its last batch.
    if (!s.touched && !resource.imported && resource.aliasPrev != UINT32_MAX) {
        const Resource& prev = m_resources[resource.aliasPrev];
        for (u32 i = 0; i < prev.stateCount; ++i) {
            const SubState& ps = m_states[prev.stateBase + i];
            for (u8 qq = 0; qq < kQueueClassCount; ++qq) {
                waitOn(qq, ps.lastBatch[qq]);
            }
        }
    }

    const bool layoutChange = resource.image && L != s.layout;

    if (s.queue != kNoQueue && s.queue != q) {
        // Cross-queue: the semaphore wait orders every earlier access on the other queues.
        for (u8 qq = 0; qq < kQueueClassCount; ++qq) {
            waitOn(qq, s.lastBatch[qq]);
        }
        const i32 releaseBatch = s.lastBatch[s.queue];
        if (s.contents && releaseBatch >= 0) {
            // EXCLUSIVE ownership transfer: release on the old queue, acquire here.
            out.srcStages = s.writeStages | s.readStages;
            out.srcAccess = s.writeAccess;
            out.dstStages = S;
            out.dstAccess = A;
            out.oldLayout = resource.image ? s.layout : 0u;
            out.newLayout = resource.image ? L : 0u;
            out.srcQueue = s.queue;
            out.dstQueue = q;
            out.releaseBatch = releaseBatch;
            need = true;
        } else if (layoutChange) {
            out.srcStages = vkc::kStageAllCommands; // chain with the semaphore wait
            out.srcAccess = 0;
            out.dstStages = S;
            out.dstAccess = A;
            out.oldLayout = s.contents ? s.layout : vkc::kLayoutUndefined;
            out.newLayout = L;
            need = true;
        }
        for (u8 qq = 0; qq < kQueueClassCount; ++qq) {
            s.lastBatch[qq] = -1;
        }
        s.writeStages = 0;
        s.writeAccess = 0;
        s.readStages = 0;
        clearVisible(s.visStages, s.visAccess);
        if (need) {
            if (W) {
                s.writeStages = S;
                s.writeAccess = A & vkc::kAccessWriteMask;
                s.contents = true;
            } else {
                s.writeStages = S; // transition / acquire chain
                s.readStages = S;
                addVisible(s.visStages, s.visAccess, S, A);
            }
        } else if (W) {
            s.writeStages = S;
            s.writeAccess = A & vkc::kAccessWriteMask;
            s.contents = true;
        } else {
            s.readStages = S;
        }
    } else if (W || layoutChange) {
        const u64 srcStages = s.writeStages | s.readStages;
        need = layoutChange || srcStages != 0u;
        if (need) {
            out.srcStages = srcStages;
            out.srcAccess = s.writeAccess;
            out.dstStages = S;
            out.dstAccess = A;
            out.oldLayout = resource.image ? (s.contents ? s.layout : vkc::kLayoutUndefined) : 0u;
            out.newLayout = resource.image ? L : 0u;
        }
        clearVisible(s.visStages, s.visAccess);
        if (W) {
            s.writeStages = S;
            s.writeAccess = A & vkc::kAccessWriteMask;
            s.readStages = 0;
            s.contents = true;
        } else {
            s.writeStages = S;
            s.writeAccess = 0;
            s.readStages = S;
            addVisible(s.visStages, s.visAccess, S, A);
        }
    } else {
        if (s.writeStages != 0u && !visible(s.visStages, s.visAccess, S, A)) {
            need = true;
            out.srcStages = s.writeStages;
            out.srcAccess = s.writeAccess;
            out.dstStages = S;
            out.dstAccess = A;
            out.oldLayout = s.layout;
            out.newLayout = s.layout;
            addVisible(s.visStages, s.visAccess, S, A);
        }
        s.readStages |= S;
    }

    if (resource.image) {
        s.layout = L;
    }
    s.queue = q;
    s.lastBatch[q] = static_cast<i32>(batchIndex);
    s.lastPass = orderPos;
    s.touched = true;
    return need;
}

void Graph::emitImage(u32 resourceId, const Pending& p, u32 mip, u32 mipCount, u32 layer, u32 layerCount,
                      u32 preStart) {
    ImageBarrier barrier;
    barrier.resource = resourceId + 1u;
    barrier.oldLayout = p.oldLayout;
    barrier.newLayout = p.newLayout;
    barrier.srcQueue = p.srcQueue;
    barrier.dstQueue = p.dstQueue;
    barrier.baseMip = mip;
    barrier.mipCount = mipCount;
    barrier.baseLayer = layer;
    barrier.layerCount = layerCount;
    if (p.releaseBatch >= 0) {
        // Ownership transfer: release (post list of the producer batch) + acquire (here), both in
        // the old layout; a layout change follows as a separate barrier on this queue.
        if (p.oldLayout != p.newLayout) {
            ImageBarrier transition = barrier;
            transition.srcQueue = kNoQueue;
            transition.dstQueue = kNoQueue;
            transition.srcStages = p.dstStages; // chains with the acquire's destination scope
            transition.srcAccess = 0;
            transition.dstStages = p.dstStages;
            transition.dstAccess = p.dstAccess;
            m_lateImageBarriers.push_back(transition);
            barrier.newLayout = p.oldLayout;
        }
        PostBarrier release;
        release.batch = static_cast<u32>(p.releaseBatch);
        release.image = true;
        release.imageBarrier = barrier;
        release.imageBarrier.srcStages = p.srcStages;
        release.imageBarrier.srcAccess = p.srcAccess;
        release.imageBarrier.dstStages = vkc::kStageNone;
        release.imageBarrier.dstAccess = 0;
        m_post.push_back(release);
        // Acquire: srcAccess is ignored; ALL_COMMANDS chains the layout transition with the
        // semaphore wait (waited at ALL_COMMANDS) that orders it after the release.
        barrier.srcStages = vkc::kStageAllCommands;
        barrier.srcAccess = 0;
        barrier.dstStages = p.dstStages;
        barrier.dstAccess = p.dstAccess;
        m_imageBarriers.push_back(barrier);
        ++m_stats.ownershipTransfers;
        return;
    }
    barrier.srcStages = p.srcStages;
    barrier.srcAccess = p.srcAccess;
    barrier.dstStages = p.dstStages;
    barrier.dstAccess = p.dstAccess;
    // Merge with the previous barrier of this pass when it covers the adjacent mip run.
    if (m_imageBarriers.size() > preStart) {
        ImageBarrier& last = m_imageBarriers.back();
        if (last.resource == barrier.resource && last.srcQueue == kNoQueue && last.srcStages == barrier.srcStages &&
            last.srcAccess == barrier.srcAccess && last.dstStages == barrier.dstStages &&
            last.dstAccess == barrier.dstAccess && last.oldLayout == barrier.oldLayout &&
            last.newLayout == barrier.newLayout && last.baseLayer == layer && last.layerCount == layerCount &&
            last.baseMip + last.mipCount == mip) {
            last.mipCount += mipCount;
            return;
        }
    }
    m_imageBarriers.push_back(barrier);
}

void Graph::planUse(const Use& use, u32 orderPos, u32 passIndex) {
    Resource& resource = m_resources[use.resource];
    const Pass& pass = m_passes[passIndex];
    const u32 batchIndex = pass.batch;

    // Merge every declaration of the same resource+range in this pass into one access.
    AccessInfo info = describeAccess(use.access, use.shaderStages, pass.queue);
    for (u32 u = pass.useBegin; u < pass.useBegin + pass.useCount; ++u) {
        const Use& other = m_uses[u];
        if (&other == &use) {
            break;
        }
        const bool same = other.resource == use.resource &&
                          (use.image ? (other.imageRange.baseMip == use.imageRange.baseMip &&
                                        other.imageRange.mipCount == use.imageRange.mipCount &&
                                        other.imageRange.baseLayer == use.imageRange.baseLayer &&
                                        other.imageRange.layerCount == use.imageRange.layerCount)
                                     : (other.bufferRange.offset == use.bufferRange.offset &&
                                        other.bufferRange.size == use.bufferRange.size));
        if (same) {
            return; // folded into the earlier declaration below
        }
    }
    for (u32 u = static_cast<u32>(&use - m_uses.data()) + 1u; u < pass.useBegin + pass.useCount; ++u) {
        const Use& other = m_uses[u];
        const bool same = other.resource == use.resource &&
                          (use.image ? (other.imageRange.baseMip == use.imageRange.baseMip &&
                                        other.imageRange.mipCount == use.imageRange.mipCount &&
                                        other.imageRange.baseLayer == use.imageRange.baseLayer &&
                                        other.imageRange.layerCount == use.imageRange.layerCount)
                                     : (other.bufferRange.offset == use.bufferRange.offset &&
                                        other.bufferRange.size == use.bufferRange.size));
        if (!same) {
            continue;
        }
        const AccessInfo more = describeAccess(other.access, other.shaderStages, pass.queue);
        info.stages |= more.stages;
        info.access |= more.access;
        info.write = info.write || more.write;
        if (use.image && more.layout != info.layout) {
            info.layout = vkc::kLayoutGeneral;
            ++m_stats.layoutConflicts;
        }
    }

    if (resource.image) {
        const u32 baseMip = std::min(use.imageRange.baseMip, resource.mips - 1u);
        const u32 mipCount = use.imageRange.mipCount == 0u ? resource.mips - baseMip
                                                           : std::min(use.imageRange.mipCount, resource.mips - baseMip);
        const u32 baseLayer = std::min(use.imageRange.baseLayer, resource.layers - 1u);
        const u32 layerCount = use.imageRange.layerCount == 0u
                                   ? resource.layers - baseLayer
                                   : std::min(use.imageRange.layerCount, resource.layers - baseLayer);
        const u32 preStart = pass.pre.imageBegin;
        for (u32 mip = baseMip; mip < baseMip + mipCount; ++mip) {
            bool runOpen = false;
            u32 runStart = 0;
            Pending run{};
            for (u32 layer = baseLayer; layer < baseLayer + layerCount; ++layer) {
                SubState& state = m_states[resource.stateBase + mip * resource.layers + layer];
                Pending pending;
                const bool need = computePending(resource, state, use, orderPos, batchIndex, info.stages, info.access,
                                                 info.layout, info.write, pending);
                if (runOpen && (!need || !(pending == run))) {
                    emitImage(use.resource, run, mip, 1u, runStart, layer - runStart, preStart);
                    runOpen = false;
                }
                if (need && !runOpen) {
                    runOpen = true;
                    runStart = layer;
                    run = pending;
                }
            }
            if (runOpen) {
                emitImage(use.resource, run, mip, 1u, runStart, baseLayer + layerCount - runStart, preStart);
            }
        }
        return;
    }

    SubState& state = m_states[resource.stateBase];
    const u64 offset = std::min(use.bufferRange.offset, resource.size);
    const u64 size = use.bufferRange.size == 0u ? resource.size - offset
                                                : std::min(use.bufferRange.size, resource.size - offset);
    u64 lo = offset;
    u64 hi = offset + size;
    if (state.touched) {
        lo = std::min(lo, state.rangeLo);
        hi = std::max(hi, state.rangeHi);
    }
    Pending pending;
    const bool need =
        computePending(resource, state, use, orderPos, batchIndex, info.stages, info.access, 0u, info.write, pending);
    state.rangeLo = lo;
    state.rangeHi = hi;
    if (!need) {
        return;
    }
    BufferBarrier barrier;
    barrier.resource = use.resource + 1u;
    barrier.srcQueue = pending.srcQueue;
    barrier.dstQueue = pending.dstQueue;
    // Whole buffer on an ownership transfer (release and acquire must match exactly and the
    // releasing batch was planned against its own ranges); otherwise every tracked access.
    barrier.offset = pending.releaseBatch >= 0 ? 0u : lo;
    barrier.size = pending.releaseBatch >= 0 ? resource.size : hi - lo;
    if (pending.releaseBatch >= 0) {
        PostBarrier release;
        release.batch = static_cast<u32>(pending.releaseBatch);
        release.image = false;
        release.bufferBarrier = barrier;
        release.bufferBarrier.srcStages = pending.srcStages;
        release.bufferBarrier.srcAccess = pending.srcAccess;
        release.bufferBarrier.dstStages = vkc::kStageNone;
        release.bufferBarrier.dstAccess = 0;
        m_post.push_back(release);
        barrier.srcStages = vkc::kStageAllCommands; // acquire: chains with the semaphore wait
        barrier.srcAccess = 0;
        ++m_stats.ownershipTransfers;
    } else {
        barrier.srcStages = pending.srcStages;
        barrier.srcAccess = pending.srcAccess;
    }
    barrier.dstStages = pending.dstStages;
    barrier.dstAccess = pending.dstAccess;
    m_bufferBarriers.push_back(barrier);
}

void Graph::emitFinalTransitions() {
    for (u32 r = 0; r < m_resources.size(); ++r) {
        const Resource& resource = m_resources[r];
        if (!resource.imported || !resource.image || resource.finalLayout == UINT32_MAX ||
            resource.life.first == UINT32_MAX) {
            continue;
        }
        for (u32 mip = 0; mip < resource.mips; ++mip) {
            for (u32 layer = 0; layer < resource.layers; ++layer) {
                SubState& s = m_states[resource.stateBase + mip * resource.layers + layer];
                if (s.layout == resource.finalLayout || s.queue == kNoQueue || s.lastBatch[s.queue] < 0) {
                    continue;
                }
                PostBarrier post;
                post.batch = static_cast<u32>(s.lastBatch[s.queue]);
                post.image = true;
                ImageBarrier& b = post.imageBarrier;
                b.resource = r + 1u;
                b.srcStages = s.writeStages | s.readStages;
                b.srcAccess = s.writeAccess;
                b.dstStages = vkc::kStageNone;
                b.dstAccess = 0;
                b.oldLayout = s.contents ? s.layout : vkc::kLayoutUndefined;
                b.newLayout = resource.finalLayout;
                b.baseMip = mip;
                b.mipCount = 1;
                b.baseLayer = layer;
                b.layerCount = 1;
                // Coalesce with the previous final transition (adjacent layer, same parameters).
                if (!m_post.empty()) {
                    PostBarrier& last = m_post.back();
                    ImageBarrier& lb = last.imageBarrier;
                    if (last.image && last.batch == post.batch && lb.resource == b.resource && lb.srcQueue == kNoQueue &&
                        lb.baseMip == mip && lb.mipCount == 1u && lb.baseLayer + lb.layerCount == layer &&
                        lb.srcStages == b.srcStages && lb.srcAccess == b.srcAccess && lb.oldLayout == b.oldLayout &&
                        lb.newLayout == b.newLayout) {
                        ++lb.layerCount;
                        s.layout = resource.finalLayout;
                        continue;
                    }
                }
                m_post.push_back(post);
                s.layout = resource.finalLayout;
            }
        }
    }
}

void Graph::flushPostBarriers() {
    for (u32 b = 0; b < m_batches.size(); ++b) {
        Batch& batch = m_batches[b];
        batch.post = {};
        batch.post.imageBegin = static_cast<u32>(m_imageBarriers.size());
        batch.post.bufferBegin = static_cast<u32>(m_bufferBarriers.size());
        for (const PostBarrier& post : m_post) {
            if (post.batch != b) {
                continue;
            }
            if (post.image) {
                m_imageBarriers.push_back(post.imageBarrier);
                ++batch.post.imageCount;
            } else {
                m_bufferBarriers.push_back(post.bufferBarrier);
                ++batch.post.bufferCount;
            }
        }
    }
}

bool Graph::plan() {
    if (!m_stats.compiled) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    m_imageBarriers.clear();
    m_lateImageBarriers.clear();
    m_bufferBarriers.clear();
    m_post.clear();
    for (Batch& batch : m_batches) {
        for (i32& wait : batch.waitBatch) {
            wait = -1;
        }
    }
    m_stats.imageBarriers = 0;
    m_stats.bufferBarriers = 0;
    m_stats.passesWithBarriers = 0;
    m_stats.ownershipTransfers = 0;
    m_stats.crossQueueWaits = 0;
    m_stats.layoutConflicts = 0;
    resetStates();

    for (u32 pos = 0; pos < m_order.size(); ++pos) {
        const u32 passIndex = m_order[pos];
        Pass& pass = m_passes[passIndex];
        pass.pre.imageBegin = static_cast<u32>(m_imageBarriers.size());
        pass.pre.bufferBegin = static_cast<u32>(m_bufferBarriers.size());
        pass.late = {};
        pass.late.imageBegin = static_cast<u32>(m_lateImageBarriers.size());
        for (u32 u = pass.useBegin; u < pass.useBegin + pass.useCount; ++u) {
            planUse(m_uses[u], pos, passIndex);
        }
        pass.late.imageCount = static_cast<u32>(m_lateImageBarriers.size()) - pass.late.imageBegin;
        pass.pre.imageCount = static_cast<u32>(m_imageBarriers.size()) - pass.pre.imageBegin;
        pass.pre.bufferCount = static_cast<u32>(m_bufferBarriers.size()) - pass.pre.bufferBegin;
        if (!pass.pre.empty()) {
            ++m_stats.passesWithBarriers;
        }
    }
    emitFinalTransitions();
    flushPostBarriers();

    for (const Resource& resource : m_resources) {
        if (!resource.imported || resource.stateCount == 0u) {
            continue;
        }
        const SubState& s = m_states[resource.stateBase];
        if (resource.layoutTracker != nullptr && resource.image) {
            *resource.layoutTracker = s.layout;
        }
        if (resource.queueTracker != nullptr) {
            *resource.queueTracker = s.queue;
        }
    }

    m_stats.imageBarriers = static_cast<u32>(m_imageBarriers.size() + m_lateImageBarriers.size());
    m_stats.bufferBarriers = static_cast<u32>(m_bufferBarriers.size());
    m_stats.planned = true;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    m_stats.compileUs += elapsed > 0 ? static_cast<u32>(elapsed) : 0u;
    return true;
}

} // namespace fuse::renderer::rg
