// Platform::getVirtualMilliseconds for tagDictionary.cpp writeHeader (probe only).
#include "platform/types.h"

#include <chrono>

namespace Platform {

U32 getVirtualMilliseconds() {
    using Clock = std::chrono::steady_clock;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch());
    return static_cast<U32>(ms.count());
}

} // namespace Platform
