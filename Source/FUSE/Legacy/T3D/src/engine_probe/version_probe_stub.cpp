// Minimal app/version symbols for FUSE_T3D_LEGACY_ENGINE_PROBE (zipArchive.cpp).
// Avoids linking version.cpp (DefineEngineFunction / engineAPI registration).

#include "platform/platform.h"
#include "app/version.h"

U32 getVersionNumber() {
    return TORQUE_GAME_ENGINE;
}

U32 getAppVersionNumber() {
    return TORQUE_APP_VERSION;
}

const char* getVersionString() {
    return TORQUE_GAME_ENGINE_VERSION_STRING;
}

const char* getAppVersionString() {
    return TORQUE_APP_VERSION_STRING;
}

const char* getEngineProductString() {
    return "Torque 3D MIT";
}

const char* getCompileTimeString() {
    return __DATE__ " at " __TIME__;
}
