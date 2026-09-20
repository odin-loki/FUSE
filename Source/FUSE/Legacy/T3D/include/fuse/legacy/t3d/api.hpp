#pragma once

#include <fuse/types.hpp>

namespace fuse::legacy::t3d {

/// Quarantined Torque3D dimension — prefixed symbols, game-thread init only (U2).
bool initialize();
void shutdown();
bool isInitialized();

/// Shim-local dynamic type IDs (subset of Torque ConsoleDynamicTypes).
namespace DynamicType {
constexpr int Bool = 1;
constexpr int S32 = 4;
constexpr int F32 = 6;
} // namespace DynamicType

namespace Con {
void init();
void execute(const char* script);
void executef(const char* fmt, ...);
void printf(const char* fmt, ...);
void errorf(const char* fmt, ...);
void warnf(const char* fmt, ...);
const char* getVariable(const char* name);
void setVariable(const char* name, const char* value);
int getIntVariable(const char* name, int def = 0);
void setIntVariable(const char* name, int value);
bool getBoolVariable(const char* name, bool def = false);
void setBoolVariable(const char* name, bool value);
void addVariable(const char* name, int type, void* pointer, const char* usage = nullptr);
void setData(int type, void* dptr, int index, int argc, const char** argv);
const char* getData(int type, void* dptr, int index = 0);
bool isFunction(const char* fn);
void threadSafeExecute(const char* script);
void addPathExpando(const char* expandoName, const char* path);
bool expandPath(char* dst, u32 size, const char* src, const char* workingDirHint = nullptr,
                bool ensureTrailingSlash = false);
void collapsePath(char* dst, u32 size, const char* src, const char* workingDirHint = nullptr);
void addConstant(const char* name, int type, const void* pointer, const char* usage = nullptr);
bool removeVariable(const char* name);
void addVariableNotify(const char* name, void (*callback)(void*), void* userdata = nullptr);
void removeVariableNotify(const char* name, void (*callback)(void*), void* userdata = nullptr);
u32 tabComplete(char* inputBuffer, u32 cursorPos, u32 maxResultLength, bool forwardTab);
const char* evaluate(const char* string, bool echo = false, const char* fileName = nullptr);
const char* evaluatef(const char* fmt, ...);
const char* executeArgv(int argc, const char** argv);
const char* executefArgv(int argc, ...);
void removePathExpando(const char* expandoName);
bool isPathExpando(const char* expandoName);
u32 getPathExpandoCount();
float getFloatVariable(const char* name, float def = 0.0f);
void setFloatVariable(const char* name, float value);
} // namespace Con

#if defined(FUSE_T3D_LEGACY_ENGINE_PROBE)
namespace engineProbe {
/// Smoke helper — calls Engine bitmapUtils extrude (probe TU linked when cmake flag ON).
void bitmapExtrude5551Smoke(const void* srcMip, void* mip, u32 srcHeight, u32 srcWidth);
void bitmapConvertRGB5551Smoke(u8* rgb, u32 pixels);
float convertHalfFloatSmoke(u16 half);
bool iesLoadEmptySmoke();
u32 md5DigestSmoke(const char* text);
u32 hash32Smoke(const char* text);
u64 hash64Smoke(const char* text);
const char* stringHash64Smoke(const char* text);
bool swizzleBgraSmoke();
u32 fourccSmoke();
bool memStreamRoundTripSmoke();
bool fileStreamTempRoundTripSmoke();
bool bitmapStbMemoryLoadSmoke();
bool readBitmapRejectsUnknownSmoke();
bool readBitmapPathSmoke();
bool writeBitmapRejectsUnknownSmoke();
bool writeBitmapStreamRoundTripSmoke();
bool writeBitmapPathSmoke();
#if defined(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG)
bool writeBitmapPngRoundTripSmoke();
#endif
bool timeClassSmoke();
bool signalSmoke();
bool crcSmoke();
bool idGeneratorSmoke();
bool bitVectorSmoke();
bool colorStaticConstSmoke();
bool stockColorSmoke();
bool dataChunkerSmoke();
bool resizeFilterStreamSmoke();
bool tagDictionarySmoke();
bool findMatchSmoke();
bool tokenizerSmoke();
bool rgb2xyzSmoke();
bool rgb2luvSmoke();
bool bitStreamRoundTripSmoke();
bool bitStreamClassIdSmoke();
bool bitStreamHuffmanStringSmoke();
bool bitStreamStringBufferSmoke();
bool stringBufferUtf8Smoke();
bool stringStartsEndsSmoke();
bool uuidRoundTripSmoke();
bool gbitmapTransparencySmoke();
bool gbitmapFillWhiteSmoke();
bool gbitmapExtensionListSmoke();
bool gbitmapColorRgba8Smoke();
bool gbitmapColorRgb8Smoke();
bool gbitmapCopyRectSmoke();
bool gbitmapExtrudeMipLevelsSmoke();
} // namespace engineProbe
#endif

u32 stringTableEntryCount();
u32 internString(const char* value);
const char* lookupString(u32 id);

} // namespace fuse::legacy::t3d
