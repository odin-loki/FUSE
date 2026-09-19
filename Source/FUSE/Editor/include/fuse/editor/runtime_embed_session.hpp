#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

/// Headless-safe embed session toward in-process `fuse_runtime` viewport (U6).
struct RuntimeEmbedSession {
    std::string projectRoot;
    std::string loadedWorldPath;
    u32 worldEntityCount = 0;
    u32 mirroredEditorEntityCount = 0;
    u32 headlessPresentTicks = 0;
    bool worldLoaded = false;
    bool headlessGpuReady = false;

    void reset();
};

} // namespace fuse::editor
