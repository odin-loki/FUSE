#include <fuse/assert.hpp>
#include <fuse/log/logger.hpp>

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace fuse::assertion {

namespace {

std::mutex g_fatalMutex;
FatalHandlerFn g_fatalHandler = nullptr;
void* g_fatalUserData = nullptr;
std::atomic<bool> g_suppressAbortForTests{false};

} // namespace

void setFatalHandler(FatalHandlerFn handler, void* userData) {
    const std::lock_guard<std::mutex> lock(g_fatalMutex);
    g_fatalHandler = handler;
    g_fatalUserData = userData;
}

void clearFatalHandler() {
    const std::lock_guard<std::mutex> lock(g_fatalMutex);
    g_fatalHandler = nullptr;
    g_fatalUserData = nullptr;
}

bool setSuppressAbortForTests(bool suppress) {
    g_suppressAbortForTests.store(suppress, std::memory_order_release);
    return suppress;
}

void fatal(const char* message, const char* file, u32 line) {
    fuse::log::Logger::instance().log(
        fuse::log::Level::Fatal, "FATAL at %s:%u — %s", file, line, message);

    FatalHandlerFn handler = nullptr;
    void* userData = nullptr;
    {
        const std::lock_guard<std::mutex> lock(g_fatalMutex);
        handler = g_fatalHandler;
        userData = g_fatalUserData;
    }

    if (handler) {
        const FatalContext context{message, file, line};
        handler(context, userData);
    }

    if (g_suppressAbortForTests.load(std::memory_order_acquire)) {
        return;
    }

    std::abort();
}

} // namespace fuse::assertion
