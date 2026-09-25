// FUSE Relight RL-2.3: command journal for the crash fallback.
// Copyright (c) 2026 FUSE contributors (MIT). New code: upstream (dxvk-remix bridge, client
// OnServerExited) shows a crash dialog and stops rendering when the server dies; FUSE instead
// rebuilds the device state in-process on plain DXVK from this journal (plan §2.6, "hang and crash
// detection with a clean fallback").
//
// The client link (link.hpp) appends every command the host accepted. When the host dies or hangs
// the journal is replayed, in order, into an in-process D3D9 executor on the passthrough d3d9.dll,
// which recreates every live object with the same client handles, every piece of uploaded data and
// the current render state; then the game continues on that device.
//
// To stay bounded, the journal is compacted at every Present (frame boundary):
// - Pure queries (Get*/Check*/Enum*/Validate*/Is*, read-backs including Lock/LockRect/LockBox) are
//   never stored; Get* rows that link a handle to an object (GetSurfaceLevel, ...) are.
// - Frame-transient work (Begin/EndScene, Clear, Draw*, StretchRect, ColorFill, Present, query
//   Issue, ...) is dropped once its frame is presented; the frame in flight is kept, so the frame
//   the host died in is re-rendered. Render-target contents of earlier frames are not restored (the
//   same contract as a D3D9 device reset: D3DPOOL_DEFAULT targets are redrawn by the game).
// - Last-writer-wins state (Set* keyed by device/object + register/stage/index) keeps the newest
//   write per key; texture/surface/volume UnlockRect/UnlockBox uploads are keyed by (object, face,
//   level, rect/box); buffer Unlock by (object, offset, size), and an Unlock with D3DLOCK_DISCARD
//   supersedes every earlier upload to that buffer. Uploads are journaled with inline data (the
//   link moves big ones to the shared heap only on the wire).
// - State-block recording (Begin/EndStateBlock), CreateStateBlock, Capture, Apply and Reset are
//   barriers: nothing before a barrier is superseded by a write after it, and recorded blocks are
//   kept verbatim.
// - An object whose Create and Destroy are both in the journal, that produced no child handles and
//   whose handle no other stored command references, disappears with all its commands.
// If the journal still grows past its byte limit it disables itself; a later host failure then
// cannot fall back (the link reports LinkMode::Failed and returns D3DERR_DEVICELOST).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fuse::relight::bridge::host {

struct JournalEntry {
    uint16_t command = 0;
    uint32_t handle = 0;
    std::vector<uint8_t> payload;  // the encoded command (schema wire format)
};

struct JournalStats {
    uint64_t appended = 0;
    uint64_t skipped = 0;           // never stored (queries, buffer Lock)
    uint64_t droppedTransient = 0;
    uint64_t droppedSuperseded = 0;
    uint64_t droppedDestroyed = 0;
    uint64_t compactions = 0;
};

class CommandJournal {
public:
    explicit CommandJournal(size_t limitBytes = size_t(128) << 20);

    // Records a command the host accepted. Returns false when the journal is (or just became)
    // disabled because it exceeded its limit.
    bool append(uint16_t command, uint32_t handle, const uint8_t* data, size_t size);

    // Calls f(const JournalEntry&) for every stored entry in order.
    template <class F>
    void replay(F&& f) const {
        for (const JournalEntry& e : entries_) {
            f(e);
        }
    }

    // Frame-boundary compaction (append() runs it on Present; tests call it directly).
    void compact();
    void clear();

    bool enabled() const noexcept { return enabled_; }
    size_t bytes() const noexcept { return bytes_; }
    size_t size() const noexcept { return entries_.size(); }
    size_t limitBytes() const noexcept { return limit_; }
    const JournalStats& stats() const noexcept { return stats_; }
    const std::vector<JournalEntry>& entries() const noexcept { return entries_; }

private:
    void push(uint16_t command, uint32_t handle, const uint8_t* data, size_t size);

    std::vector<JournalEntry> entries_;
    size_t bytes_ = 0;
    size_t limit_;
    bool enabled_ = true;
    JournalStats stats_;
};

}  // namespace fuse::relight::bridge::host
