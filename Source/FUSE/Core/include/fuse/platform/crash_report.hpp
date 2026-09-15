#pragma once

#include <functional>

#include <fuse/types.hpp>

namespace fuse::platform {

struct CrashReportContext {
    const char* message = nullptr;
    const char* file = nullptr;
    u32 line = 0;
};

using CrashReportCallback = std::function<void(const CrashReportContext& context)>;

/// Install platform crash handlers (stub — no OS signal wiring yet; B7.8 follow-up).
bool installCrashHandlers();

void shutdownCrashHandlers();

void setCrashReportCallback(CrashReportCallback callback);

/// Manual report path for tests and non-fatal diagnostics.
void submitCrashReport(const CrashReportContext& context);

bool crashHandlersInstalled();

} // namespace fuse::platform
