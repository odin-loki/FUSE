#pragma once

namespace fuse::core {

/// Initialise FUSE core (jobs scheduler, platform hooks). Safe to call once.
bool initialize();

/// Tear down core services. No-op if not initialized.
void shutdown();

bool isInitialized();

} // namespace fuse::core
