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

// FUSE Relight RL-2.1: the shared heap — large resource data (texture/buffer locks) that does not
// go through the data ring.
//
// Semantics kept from upstream SharedHeap: the heap is split into fixed-size chunks spread over
// shared-memory segments that are added on demand; one side (the client, "owner") allocates runs of
// chunks and keeps the allocation map; the other side (the host, "peer") only reads the data and
// marks an allocation Deallocated in a shared per-chunk state array; the owner reclaims Deallocated
// runs lazily when it needs space and waits (with a timeout) for the peer to free chunks when the
// heap is full; first-fit in the gaps between allocations, then at the end, then a new segment; an
// allocation never crosses a segment boundary.
//
// Changes (revamp):
// - One implementation for both roles (upstream compiled two halves with REMIX_BRIDGE_CLIENT /
//   REMIX_BRIDGE_SERVER) and no process-wide singleton: a session owns its heap.
// - Allocations are addressed by HeapRef {firstChunk, bytes}, which a command carries inline
//   (message flag kDataInSharedHeap). Upstream allocated a separate id, sent Bridge_SharedHeap_Alloc /
//   _Dealloc / _AddSeg commands, and kept id->chunk caches on both sides. Here the segment table
//   lives in the heap's shared meta region, so the peer maps new segments on first use and needs no
//   command; release() is a CAS on the shared chunk state (double release is detected).
// - The gap search clips each gap to segment boundaries and keeps searching; upstream returned the
//   first gap that was big enough and rejected it if it crossed a segment boundary, even when a
//   later gap would have fit.
// - The peer validates every HeapRef (range, state, segment) before returning a pointer.
// - Waiting uses Waiter: peer death/close ends the wait (upstream waited sharedHeapFreeChunkWaitTimeout
//   seconds regardless).
// - The meta region is sized by the configured maximum (upstream always reserved 2 GiB / chunk
//   size states); an oversized request grows the segment size like upstream's default-size bump.
#pragma once

#include <fuse/relight/bridge/ipc/platform.hpp>
#include <fuse/relight/bridge/ipc/result.hpp>
#include <fuse/relight/bridge/ipc/wait.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fuse::relight::bridge::ipc {

struct HeapConfig {
    uint32_t chunkBytes = 4096;          // power of two
    uint32_t segmentBytes = 16u << 20;   // default segment size, multiple of chunkBytes
    uint32_t maxBytes = 512u << 20;      // total cap (the x86 client's address space is the limit)
};

struct HeapRef {
    uint32_t firstChunk = 0xFFFFFFFFu;
    uint32_t bytes = 0;
    bool valid() const noexcept { return firstChunk != 0xFFFFFFFFu; }
};

struct HeapStats {
    uint32_t segments = 0;
    uint64_t capacityBytes = 0;
    uint64_t allocatedBytes = 0;  // owner view, including peer-released runs not yet reclaimed
    uint32_t allocations = 0;
};

class SharedHeap {
public:
    static constexpr uint32_t kMaxSegments = 64;

    enum class ChunkState : uint8_t { Unallocated = 0, Allocated = 1, Deallocated = 2 };

    SharedHeap() = default;
    SharedHeap(const SharedHeap&) = delete;
    SharedHeap& operator=(const SharedHeap&) = delete;

    // Owner (client): creates the meta region `<name>.hm` and the first segment.
    Result create(const std::string& name, const HeapConfig& config);
    // Peer (host): maps the meta region; segments are mapped on demand.
    Result open(const std::string& name);
    void setWaitContext(const WaitContext* ctx) noexcept { ctx_ = ctx; }
    bool isOwner() const noexcept { return owner_; }
    bool valid() const noexcept { return meta_.valid(); }

    // ---- owner ---------------------------------------------------------------------------------
    // Allocates `bytes`, reclaiming peer-released runs and adding segments as needed; when the heap is
    // at its cap, waits for the peer to release (back-pressure) up to timeoutMs.
    Result allocate(uint32_t bytes, HeapRef& out, uint32_t timeoutMs);
    // Owner-side free of an allocation the peer never received (e.g. its command failed to send).
    void freeLocal(const HeapRef& ref);
    // Reclaims every run the peer released. Returns the number reclaimed.
    uint32_t collect();
    uint8_t* data(const HeapRef& ref);
    HeapStats stats() const;

    // ---- peer ----------------------------------------------------------------------------------
    // Validated pointer to an Allocated run; nullptr if the reference is not valid.
    const uint8_t* resolve(const HeapRef& ref);
    // Marks the run Deallocated for the owner to reclaim. False on an invalid or repeated release.
    bool release(const HeapRef& ref);

    uint32_t chunkBytes() const noexcept { return chunkBytes_; }

private:
    struct Meta;
    struct Segment {
        SharedMemory mem;
        uint32_t baseChunk = 0;
        uint32_t chunks = 0;
    };

    Meta* meta() const noexcept;
    uint8_t* states() const noexcept;
    ChunkState state(uint32_t chunk) const noexcept;
    void setState(uint32_t chunk, ChunkState s) noexcept;
    bool addSegment(uint32_t minChunks);
    bool mapSegments();
    int segmentOf(uint32_t chunk) const noexcept;
    bool findRun(uint32_t n, uint32_t& first) const;
    std::string segmentName(uint32_t index) const;

    std::string name_;
    bool owner_ = false;
    SharedMemory meta_;
    std::vector<Segment> segments_;
    uint32_t chunkBytes_ = 0;
    uint32_t maxChunks_ = 0;
    uint32_t segmentChunks_ = 0;       // current default segment size in chunks (owner)
    uint32_t totalChunks_ = 0;         // chunks covered by mapped segments
    std::map<uint32_t, uint32_t> allocations_;  // owner: first chunk -> last chunk
    uint64_t allocatedBytes_ = 0;
    const WaitContext* ctx_ = nullptr;
};

}  // namespace fuse::relight::bridge::ipc
