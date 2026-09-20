#pragma once

// macOS CoreServices / FSEvents API stub (not linked in umbrella build).
// Ore: FSEventStreamCreate + kFSEventStreamEventFlagFileEvents without CoreServices.framework.

#include <fuse/types.hpp>

namespace fuse::ai::uaisk::fsevents_stub {

using FSEventStreamRef = void*;
using CFRunLoopRef = void*;

constexpr u32 kFSEventStreamEventFlagFileEvents = 0x00000001u;
constexpr u32 kFSEventStreamCreateFlagFileEvents = 0x00000001u;
constexpr u32 kFSEventStreamCreateFlagNoDefer = 0x00000002u;
constexpr u64 kFSEventStreamEventIdSinceNow = 0xFFFFFFFFFFFFFFFFull;

struct CoreServicesWatchConfig {
    u32 latencyMs = 16;
    u32 coalesceThreshold = 4;
    u32 createFlags = kFSEventStreamCreateFlagFileEvents;
    u64 sinceEventId = kFSEventStreamEventIdSinceNow;
};

[[nodiscard]] inline CoreServicesWatchConfig makeDefaultCoreServicesWatchConfig() {
    return CoreServicesWatchConfig{};
}

/// Stub FSEventStreamCreate signature — returns nullptr when CoreServices is not linked.
[[nodiscard]] inline FSEventStreamRef createFileEventStreamStub(const char* /*path*/,
                                                                CoreServicesWatchConfig config) {
    (void)config;
    return nullptr;
}

/// Stub release — no-op when stream ref is null.
inline void releaseFileEventStreamStub(FSEventStreamRef stream) {
    (void)stream;
}

} // namespace fuse::ai::uaisk::fsevents_stub
