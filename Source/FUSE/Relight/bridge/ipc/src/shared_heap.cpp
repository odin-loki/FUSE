/*
 * Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix bridge/src/util/util_sharedheap.{h,cpp}@0867d3c

#include <fuse/relight/bridge/ipc/shared_heap.hpp>

#include <atomic>
#include <cstring>
#include <new>
#include <utility>

namespace fuse::relight::bridge::ipc {

// Shared meta region: header + segment table + one state byte per chunk.
struct SharedHeap::Meta {
    uint32_t magic;
    uint32_t version;
    uint32_t chunkBytes;
    uint32_t maxChunks;
    std::atomic<uint32_t> segmentCount;  // published after the segment's table entry is written
    uint32_t reserved[3];
    uint32_t segmentBase[kMaxSegments];
    uint32_t segmentChunks[kMaxSegments];
};
static_assert(sizeof(std::atomic<uint8_t>) == 1, "chunk states are single shared bytes");

namespace {
constexpr uint32_t kHeapMagic = 0x50484C46u;  // "FLHP"
constexpr uint32_t kHeapVersion = 1;
constexpr size_t kMetaHeaderBytes = 32 + 2 * 4 * SharedHeap::kMaxSegments;
}  // namespace

SharedHeap::Meta* SharedHeap::meta() const noexcept { return static_cast<Meta*>(meta_.data()); }

uint8_t* SharedHeap::states() const noexcept { return static_cast<uint8_t*>(meta_.data()) + kMetaHeaderBytes; }

SharedHeap::ChunkState SharedHeap::state(uint32_t chunk) const noexcept {
    auto* s = reinterpret_cast<std::atomic<uint8_t>*>(states() + chunk);
    return static_cast<ChunkState>(s->load(std::memory_order_acquire));
}

void SharedHeap::setState(uint32_t chunk, ChunkState st) noexcept {
    auto* s = reinterpret_cast<std::atomic<uint8_t>*>(states() + chunk);
    s->store(static_cast<uint8_t>(st), std::memory_order_release);
}

std::string SharedHeap::segmentName(uint32_t index) const { return name_ + ".h" + std::to_string(index); }

Result SharedHeap::create(const std::string& name, const HeapConfig& config) {
    static_assert(sizeof(Meta) == kMetaHeaderBytes, "heap meta layout must match across x86/x64");
    if (!isPowerOfTwo(config.chunkBytes) || config.segmentBytes < config.chunkBytes ||
        config.segmentBytes % config.chunkBytes != 0 || config.maxBytes < config.segmentBytes) {
        return Result::Malformed;
    }
    name_ = name;
    owner_ = true;
    chunkBytes_ = config.chunkBytes;
    maxChunks_ = config.maxBytes / config.chunkBytes;
    segmentChunks_ = config.segmentBytes / config.chunkBytes;
    Result r = meta_.create(name + ".hm", kMetaHeaderBytes + maxChunks_);
    if (r != Result::Success) {
        return r;
    }
    Meta* m = new (meta_.data()) Meta();
    m->magic = kHeapMagic;
    m->version = kHeapVersion;
    m->chunkBytes = chunkBytes_;
    m->maxChunks = maxChunks_;
    m->segmentCount.store(0, std::memory_order_relaxed);
    // States are zero (Unallocated) from the zero-filled mapping.
    return addSegment(segmentChunks_) ? Result::Success : Result::Failure;
}

Result SharedHeap::open(const std::string& name) {
    name_ = name;
    owner_ = false;
    Result r = meta_.open(name + ".hm", kMetaHeaderBytes);
    if (r != Result::Success) {
        return r;
    }
    meta_.unlinkName();  // both sides mapped it: nothing leaks if either process dies
    const Meta* m = meta();
    if (m->magic != kHeapMagic || m->version != kHeapVersion || !isPowerOfTwo(m->chunkBytes) ||
        meta_.size() < kMetaHeaderBytes + size_t(m->maxChunks)) {
        return Result::VersionMismatch;
    }
    chunkBytes_ = m->chunkBytes;
    maxChunks_ = m->maxChunks;
    return mapSegments() ? Result::Success : Result::Failure;
}

bool SharedHeap::addSegment(uint32_t minChunks) {
    Meta* m = meta();
    const uint32_t index = static_cast<uint32_t>(segments_.size());
    if (index >= kMaxSegments || totalChunks_ >= maxChunks_) {
        return false;
    }
    uint32_t chunks = segmentChunks_ > minChunks ? segmentChunks_ : minChunks;
    if (chunks > maxChunks_ - totalChunks_) {
        chunks = maxChunks_ - totalChunks_;
    }
    if (chunks < minChunks) {
        return false;
    }
    Segment seg;
    // Like upstream, halve the request while the OS refuses the mapping.
    while (seg.mem.create(segmentName(index), size_t(chunks) * chunkBytes_) != Result::Success) {
        if (chunks / 2 < minChunks || chunks < 2) {
            return false;
        }
        chunks /= 2;
    }
    seg.baseChunk = totalChunks_;
    seg.chunks = chunks;
    m->segmentBase[index] = seg.baseChunk;
    m->segmentChunks[index] = chunks;
    m->segmentCount.store(index + 1, std::memory_order_release);
    totalChunks_ += chunks;
    segments_.push_back(std::move(seg));
    return true;
}

bool SharedHeap::mapSegments() {
    const Meta* m = meta();
    uint32_t count = m->segmentCount.load(std::memory_order_acquire);
    if (count > kMaxSegments) {
        return false;
    }
    while (segments_.size() < count) {
        const uint32_t index = static_cast<uint32_t>(segments_.size());
        Segment seg;
        seg.baseChunk = m->segmentBase[index];
        seg.chunks = m->segmentChunks[index];
        if (seg.baseChunk != totalChunks_ || seg.chunks == 0 || uint64_t(seg.baseChunk) + seg.chunks > maxChunks_) {
            return false;
        }
        if (seg.mem.open(segmentName(index), size_t(seg.chunks) * chunkBytes_) != Result::Success) {
            return false;
        }
        seg.mem.unlinkName();
        totalChunks_ += seg.chunks;
        segments_.push_back(std::move(seg));
    }
    return true;
}

int SharedHeap::segmentOf(uint32_t chunk) const noexcept {
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (chunk >= segments_[i].baseChunk && chunk - segments_[i].baseChunk < segments_[i].chunks) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SharedHeap::findRun(uint32_t n, uint32_t& first) const {
    // Walk the gaps between allocations (and after the last), clipped to segment bounds.
    auto fitInGap = [&](uint32_t gapBegin, uint32_t gapEnd) {
        for (const Segment& s : segments_) {
            const uint32_t b = gapBegin > s.baseChunk ? gapBegin : s.baseChunk;
            const uint32_t segEnd = s.baseChunk + s.chunks;
            const uint32_t e = gapEnd < segEnd ? gapEnd : segEnd;
            if (b < e && e - b >= n) {
                first = b;
                return true;
            }
        }
        return false;
    };
    uint32_t cursor = 0;
    for (const auto& [a, z] : allocations_) {
        if (a > cursor && fitInGap(cursor, a)) {
            return true;
        }
        cursor = z + 1;
    }
    return cursor < totalChunks_ && fitInGap(cursor, totalChunks_);
}

uint32_t SharedHeap::collect() {
    uint32_t n = 0;
    for (auto it = allocations_.begin(); it != allocations_.end();) {
        if (state(it->first) == ChunkState::Deallocated) {
            allocatedBytes_ -= uint64_t(it->second - it->first + 1) * chunkBytes_;
            setState(it->first, ChunkState::Unallocated);
            it = allocations_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

Result SharedHeap::allocate(uint32_t bytes, HeapRef& out, uint32_t timeoutMs) {
    if (!owner_ || bytes == 0) {
        return Result::Failure;
    }
    const uint32_t n = (bytes + chunkBytes_ - 1) / chunkBytes_;
    if (n > maxChunks_) {
        return Result::TooLarge;
    }
    if (n > segmentChunks_) {
        // Upstream: an allocation larger than the default segment doubles the default size.
        while (segmentChunks_ < n && segmentChunks_ < maxChunks_ / 2) {
            segmentChunks_ *= 2;
        }
    }
    Waiter waiter(ctx_, timeoutMs);
    uint32_t first = 0;
    for (;;) {
        if (findRun(n, first) || (collect() > 0 && findRun(n, first))) {
            break;
        }
        if (addSegment(n)) {
            first = segments_.back().baseChunk;
            break;
        }
        // Segments never merge: once no segment can be added, a run longer than every segment can
        // never be satisfied, so fail now instead of waiting for releases that cannot help.
        uint32_t largest = 0;
        for (const Segment& s : segments_) {
            largest = s.chunks > largest ? s.chunks : largest;
        }
        if (largest < n) {
            return Result::TooLarge;
        }
        const Result r = waiter.step();
        if (r != Result::Success) {
            return r;
        }
    }
    allocations_[first] = first + n - 1;
    allocatedBytes_ += uint64_t(n) * chunkBytes_;
    setState(first, ChunkState::Allocated);
    out.firstChunk = first;
    out.bytes = bytes;
    return Result::Success;
}

void SharedHeap::freeLocal(const HeapRef& ref) {
    auto it = allocations_.find(ref.firstChunk);
    if (!owner_ || it == allocations_.end()) {
        return;
    }
    allocatedBytes_ -= uint64_t(it->second - it->first + 1) * chunkBytes_;
    setState(it->first, ChunkState::Unallocated);
    allocations_.erase(it);
}

uint8_t* SharedHeap::data(const HeapRef& ref) {
    const int s = segmentOf(ref.firstChunk);
    if (s < 0) {
        return nullptr;
    }
    const Segment& seg = segments_[size_t(s)];
    return static_cast<uint8_t*>(seg.mem.data()) + size_t(ref.firstChunk - seg.baseChunk) * chunkBytes_;
}

const uint8_t* SharedHeap::resolve(const HeapRef& ref) {
    if (!ref.valid() || ref.bytes == 0 || ref.firstChunk >= maxChunks_) {
        return nullptr;
    }
    int s = segmentOf(ref.firstChunk);
    if (s < 0) {
        if (!mapSegments()) {
            return nullptr;
        }
        s = segmentOf(ref.firstChunk);
        if (s < 0) {
            return nullptr;
        }
    }
    const Segment& seg = segments_[size_t(s)];
    const uint64_t chunks = (uint64_t(ref.bytes) + chunkBytes_ - 1) / chunkBytes_;
    if (ref.firstChunk - seg.baseChunk + chunks > seg.chunks || state(ref.firstChunk) != ChunkState::Allocated) {
        return nullptr;
    }
    return static_cast<const uint8_t*>(seg.mem.data()) + size_t(ref.firstChunk - seg.baseChunk) * chunkBytes_;
}

bool SharedHeap::release(const HeapRef& ref) {
    if (!ref.valid() || ref.firstChunk >= maxChunks_) {
        return false;
    }
    auto* s = reinterpret_cast<std::atomic<uint8_t>*>(states() + ref.firstChunk);
    uint8_t expected = static_cast<uint8_t>(ChunkState::Allocated);
    return s->compare_exchange_strong(expected, static_cast<uint8_t>(ChunkState::Deallocated), std::memory_order_acq_rel);
}

HeapStats SharedHeap::stats() const {
    HeapStats st;
    st.segments = static_cast<uint32_t>(segments_.size());
    st.capacityBytes = uint64_t(totalChunks_) * chunkBytes_;
    st.allocatedBytes = allocatedBytes_;
    st.allocations = static_cast<uint32_t>(allocations_.size());
    return st;
}

}  // namespace fuse::relight::bridge::ipc
