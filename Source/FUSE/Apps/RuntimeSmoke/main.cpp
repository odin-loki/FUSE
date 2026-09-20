#include <fuse/core/init.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/legacy/parallel_for.hpp>
#include <fuse/legacy/t2d/api.hpp>
#include <fuse/legacy/t3d/api.hpp>
#include <fuse/legacy/t3d/image_compress.hpp>
#include <fuse/legacy/t3d/sim_object_bridge.hpp>
#include <fuse/legacy/t2d/sim_object_bridge.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/renderer/renderer_bootstrap.hpp>
#endif

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "SMOKE FAIL: %s\n", message);
        ++g_failures;
    }
}

void runJobSmoke() {
    std::atomic<fuse::u32> sum{0};
    fuse::jobs::parallel_for(0u, 100u, 10u, [&sum](fuse::u32 i) {
        sum.fetch_add(i, std::memory_order_relaxed);
    });
    check(sum.load(std::memory_order_acquire) == 4950u, "parallel_for sum matches serial expectation");
}

void runLegacyConSmoke() {
    fuse::legacy::t3d::Con::setVariable("$prefixed", "t3d");
    fuse::legacy::t2d::Con::setVariable("$prefixed", "t2d");
    check(std::strcmp(fuse::legacy::t3d::Con::getVariable("$FuseT3D"), "1") == 0, "t3d boot variable set");
    check(std::strcmp(fuse::legacy::t2d::Con::getVariable("$FuseT2D"), "1") == 0, "t2d boot variable set");
    check(std::strcmp(fuse::legacy::t3d::Con::getVariable("$prefixed"), "t3d") == 0, "t3d variable round-trip");
    check(std::strcmp(fuse::legacy::t2d::Con::getVariable("$prefixed"), "t2d") == 0, "t2d variable round-trip");

    fuse::legacy::t3d::Con::warnf("[t3d] warnf shim");
    fuse::legacy::t2d::Con::warnf("[t2d] warnf shim");
    fuse::legacy::t3d::Con::errorf("[t3d] errorf shim");
    fuse::legacy::t2d::Con::errorf("[t2d] errorf shim");
    fuse::legacy::t3d::Con::executef("echo %s", "T3D executef");
    fuse::legacy::t2d::Con::executef("echo %s", "T2D executef");

    fuse::legacy::t3d::Con::setIntVariable("$SmokeInt", 42);
    fuse::legacy::t2d::Con::setIntVariable("$SmokeInt", 24);
    check(fuse::legacy::t3d::Con::getIntVariable("$SmokeInt", 0) == 42, "t3d int variable round-trip");
    check(fuse::legacy::t2d::Con::getIntVariable("$SmokeInt", 0) == 24, "t2d int variable round-trip");
    check(fuse::legacy::t3d::Con::getIntVariable("$MissingInt", 7) == 7, "t3d int default preserved");
    check(fuse::legacy::t2d::Con::getIntVariable("$MissingInt", 9) == 9, "t2d int default preserved");

    fuse::legacy::t3d::Con::setBoolVariable("$SmokeBool", true);
    fuse::legacy::t2d::Con::setBoolVariable("$SmokeBool", false);
    check(fuse::legacy::t3d::Con::getBoolVariable("$SmokeBool", false), "t3d bool variable round-trip");
    check(!fuse::legacy::t2d::Con::getBoolVariable("$SmokeBool", true), "t2d bool variable round-trip");

    char expanded[256] = {};
    check(fuse::legacy::t3d::Con::expandPath(expanded, sizeof(expanded), "^game/textures/foo.png"),
          "t3d expandPath resolves ^game expando");
    check(std::strcmp(expanded, "/game/textures/foo.png") == 0, "t3d expandPath output matches");
    check(fuse::legacy::t2d::Con::expandPath(expanded, sizeof(expanded), "^game/textures/foo.png"),
          "t2d expandPath resolves ^game expando");
    check(std::strcmp(expanded, "/game/textures/foo.png") == 0, "t2d expandPath output matches");

    fuse::legacy::t3d::Con::collapsePath(expanded, sizeof(expanded), "/game/textures/foo.png");
    check(std::strcmp(expanded, "^game/textures/foo.png") == 0, "t3d collapsePath round-trip");
    fuse::legacy::t2d::Con::collapsePath(expanded, sizeof(expanded), "/game/textures/foo.png");
    check(std::strcmp(expanded, "^game/textures/foo.png") == 0, "t2d collapsePath round-trip");

    int t3dBound = 7;
    int t2dBound = 9;
    fuse::legacy::t3d::Con::addVariable("$BoundInt", fuse::legacy::t3d::DynamicType::S32, &t3dBound);
    fuse::legacy::t2d::Con::addVariable("$BoundInt", fuse::legacy::t2d::DynamicType::S32, &t2dBound);
    check(t3dBound == 7, "t3d addVariable initial sync");
    check(t2dBound == 9, "t2d addVariable initial sync");
    fuse::legacy::t3d::Con::setVariable("$BoundInt", "42");
    fuse::legacy::t2d::Con::setVariable("$BoundInt", "24");
    check(t3dBound == 42, "t3d addVariable setVariable round-trip");
    check(t2dBound == 24, "t2d addVariable setVariable round-trip");

    float t3dFloat = 1.5f;
    const char* t3dFloatArgv[] = {"2.75"};
    fuse::legacy::t3d::Con::setData(fuse::legacy::t3d::DynamicType::F32, &t3dFloat, 0, 1, t3dFloatArgv);
    check(t3dFloat > 2.74f && t3dFloat < 2.76f, "t3d setData F32 round-trip");
    const char* t3dFloatOut = fuse::legacy::t3d::Con::getData(fuse::legacy::t3d::DynamicType::F32, &t3dFloat, 0);
    check(t3dFloatOut != nullptr && t3dFloatOut[0] != '\0', "t3d getData F32 non-empty");

    check(fuse::legacy::t3d::Con::isFunction("legacyBoot"), "t3d isFunction legacyBoot");
    check(!fuse::legacy::t3d::Con::isFunction("notARealFn"), "t3d isFunction rejects unknown");
    check(fuse::legacy::t2d::Con::isFunction("legacyBoot"), "t2d isFunction legacyBoot");

    fuse::legacy::t3d::Con::threadSafeExecute("echo threadSafe T3D");
    fuse::legacy::t2d::Con::threadSafeExecute("echo threadSafe T2D");

    fuse::legacy::t3d::Con::setFloatVariable("$SmokeFloat", 3.5f);
    fuse::legacy::t2d::Con::setFloatVariable("$SmokeFloat", 2.25f);
    check(fuse::legacy::t3d::Con::getFloatVariable("$SmokeFloat", 0.0f) > 3.4f, "t3d float variable round-trip");
    check(fuse::legacy::t2d::Con::getFloatVariable("$SmokeFloat", 0.0f) > 2.2f, "t2d float variable round-trip");

    check(fuse::legacy::t3d::Con::isPathExpando("game"), "t3d isPathExpando game");
    check(fuse::legacy::t2d::Con::isPathExpando("game"), "t2d isPathExpando game");
    check(fuse::legacy::t3d::Con::getPathExpandoCount() >= 1u, "t3d path expando count");
    check(fuse::legacy::t2d::Con::getPathExpandoCount() >= 1u, "t2d path expando count");
    fuse::legacy::t3d::Con::removePathExpando("game");
    fuse::legacy::t2d::Con::removePathExpando("game");
    check(!fuse::legacy::t3d::Con::isPathExpando("game"), "t3d removePathExpando");
    check(!fuse::legacy::t2d::Con::isPathExpando("game"), "t2d removePathExpando");
    fuse::legacy::t3d::Con::addPathExpando("game", "/game");
    fuse::legacy::t2d::Con::addPathExpando("game", "/game");

    const int t3dConst = 99;
    fuse::legacy::t3d::Con::addConstant("$SmokeConst", fuse::legacy::t3d::DynamicType::S32, &t3dConst);
    fuse::legacy::t2d::Con::addConstant("$SmokeConst", fuse::legacy::t2d::DynamicType::S32, &t3dConst);
    check(std::strcmp(fuse::legacy::t3d::Con::getVariable("$SmokeConst"), "99") == 0, "t3d addConstant");
    fuse::legacy::t3d::Con::setVariable("$SmokeConst", "1");
    check(std::strcmp(fuse::legacy::t3d::Con::getVariable("$SmokeConst"), "99") == 0,
          "t3d addConstant rejects setVariable");

    fuse::legacy::t3d::Con::setVariable("$NotifyVar", "before");
    std::atomic<int> notifyCount{0};
    auto notifyCb = +[](void* ctx) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    };
    fuse::legacy::t3d::Con::addVariableNotify("$NotifyVar", notifyCb, &notifyCount);
    fuse::legacy::t3d::Con::setVariable("$NotifyVar", "after");
    check(notifyCount.load(std::memory_order_acquire) == 1, "t3d addVariableNotify fires");
    fuse::legacy::t3d::Con::removeVariableNotify("$NotifyVar", notifyCb, &notifyCount);
    check(fuse::legacy::t3d::Con::removeVariable("$NotifyVar"), "t3d removeVariable");

    char tabBuffer[64] = "leg";
    const fuse::u32 tabPos =
        fuse::legacy::t3d::Con::tabComplete(tabBuffer, 3, sizeof(tabBuffer), true);
    check(tabPos > 3u, "t3d tabComplete extends legacyBoot prefix");
    check(std::strncmp(tabBuffer, "legacyBoot", 10) == 0, "t3d tabComplete output");

    const char* evalResult = fuse::legacy::t3d::Con::evaluate("1+1;", false, nullptr);
    check(evalResult != nullptr, "t3d evaluate returns buffer");
    const char* evalfResult = fuse::legacy::t3d::Con::evaluatef("%s", "2+2;");
    check(evalfResult != nullptr, "t3d evaluatef returns buffer");

    const char* execArgv[] = {"echo", "argv", "T3D"};
    const char* execArgvResult = fuse::legacy::t3d::Con::executeArgv(3, execArgv);
    check(execArgvResult != nullptr, "t3d executeArgv returns buffer");
    const char* execfArgvResult = fuse::legacy::t3d::Con::executefArgv(2, "echo", "executefArgv T3D");
    check(execfArgvResult != nullptr, "t3d executefArgv returns buffer");
}

void runStringInternSmoke() {
    char dynamicT3d[64];
    char dynamicT2d[64];
    std::snprintf(dynamicT3d, sizeof(dynamicT3d), "t3d_dynamic_%d", 7);
    std::snprintf(dynamicT2d, sizeof(dynamicT2d), "t2d_dynamic_%d", 9);

    const fuse::u32 t3dId = fuse::legacy::t3d::internString(dynamicT3d);
    const fuse::u32 t2dId = fuse::legacy::t2d::internString(dynamicT2d);
    check(t3dId != 0u, "t3d intern dynamic string");
    check(t2dId != 0u, "t2d intern dynamic string");
    check(std::strcmp(fuse::legacy::t3d::lookupString(t3dId), dynamicT3d) == 0,
          "t3d lookup survives transient buffer");
    check(std::strcmp(fuse::legacy::t2d::lookupString(t2dId), dynamicT2d) == 0,
          "t2d lookup survives transient buffer");
    check(fuse::legacy::t3d::internString(dynamicT3d) == t3dId, "t3d intern idempotent");
    check(fuse::legacy::t2d::internString(dynamicT2d) == t2dId, "t2d intern idempotent");
}

void runSimObjectBridgeSmoke() {
    fuse::platform::registerMainThread();

    fuse::legacy::LegacySimObjectStub t3dLegacy{};
    t3dLegacy.simObjectId = 101;
    t3dLegacy.className = "StaticShape";
    t3dLegacy.internalName = "spawnPoint1";
    t3dLegacy.name = "Spawn";
    t3dLegacy.x = 3.f;
    t3dLegacy.y = 4.f;
    t3dLegacy.z = 5.f;

    fuse::SceneObject3D imported3d("placeholder");
    check(fuse::legacy::t3d::importSimObject(t3dLegacy, imported3d), "t3d SimObject bridge import");
    fuse::legacy::LegacySimObjectStub exported3d{};
    check(fuse::legacy::t3d::exportSimObject(imported3d, exported3d), "t3d SimObject bridge export");
    check(exported3d.simObjectId == 101u, "t3d SimObject bridge legacyId");
    check(exported3d.name == "Spawn", "t3d SimObject bridge name");
    check(exported3d.className == "StaticShape", "t3d SimObject bridge className");
    check(exported3d.internalName == "spawnPoint1", "t3d SimObject bridge internalName");
    check(exported3d.x > 2.9f && exported3d.x < 3.1f, "t3d SimObject bridge x");

    fuse::legacy::LegacySimObjectStub t2dLegacy{};
    t2dLegacy.simObjectId = 55;
    t2dLegacy.className = "t2dSceneObject";
    t2dLegacy.internalName = "playerSprite";
    t2dLegacy.name = "Player";
    t2dLegacy.x = 8.f;
    t2dLegacy.y = 9.f;
    t2dLegacy.layer = 2;

    fuse::SceneObject2D imported2d("placeholder");
    check(fuse::legacy::t2d::importSimObject(t2dLegacy, imported2d), "t2d SimObject bridge import");
    fuse::legacy::LegacySimObjectStub exported2d{};
    check(fuse::legacy::t2d::exportSimObject(imported2d, exported2d), "t2d SimObject bridge export");
    check(exported2d.simObjectId == 55u, "t2d SimObject bridge legacyId");
    check(exported2d.layer == 2, "t2d SimObject bridge layer");
    check(exported2d.className == "t2dSceneObject", "t2d SimObject bridge className");
    check(exported2d.internalName == "playerSprite", "t2d SimObject bridge internalName");

    t3dLegacy.parentName = "MissionGroup";
    fuse::SceneObject3D parented("parented");
    check(fuse::legacy::t3d::importSimObject(t3dLegacy, parented), "t3d SimObject bridge parentName import");
    fuse::legacy::LegacySimObjectStub parentedOut{};
    check(fuse::legacy::t3d::exportSimObject(parented, parentedOut), "t3d SimObject bridge parentName export");
    check(parentedOut.parentName == "MissionGroup", "t3d SimObject bridge parentName round-trip");
}

void runImageCompressSmoke() {
    constexpr fuse::u32 kWidth = 8;
    constexpr fuse::u32 kHeight = 8;
    constexpr fuse::u32 kMipCount = 2;

    std::vector<fuse::u8> srcRGBA(kWidth * kHeight * 4, 0xAB);
    std::vector<fuse::legacy::t3d::image::MipLevel> mips(kMipCount);
    std::vector<std::vector<fuse::u8>> dstStorage(kMipCount);

    for (fuse::u32 mip = 0; mip < kMipCount; ++mip) {
        const fuse::u32 mipWidth = kWidth >> mip;
        const fuse::u32 mipHeight = kHeight >> mip;
        dstStorage[mip].resize(fuse::legacy::t3d::image::compressedMipByteCount(
            mipWidth, mipHeight, fuse::legacy::t3d::image::CompressFormat::BC1));
        mips[mip].srcRGBA = srcRGBA.data();
        mips[mip].dst = dstStorage[mip].data();
        mips[mip].width = mipWidth;
        mips[mip].height = mipHeight;
    }

    check(fuse::legacy::t3d::image::compressMipsParallel(
              mips.data(), kMipCount, fuse::legacy::t3d::image::CompressFormat::BC1,
              fuse::legacy::t3d::image::CompressQuality::Low),
          "parallel mip compress completes");

    const fuse::u32 checksum =
        fuse::legacy::t3d::image::compressedMipsChecksum(mips.data(), kMipCount,
                                                        fuse::legacy::t3d::image::CompressFormat::BC1);
    check(checksum != 0u, "compressed mip checksum non-zero");
}

} // namespace

int main() {
    fuse::log::info("fuse_runtime_smoke: starting one-process quarantine test");

    check(fuse::core::initialize(), "fuse_core initialize");
    check(fuse::jobs::JobScheduler::instance().isInitialized(), "job scheduler initialized");

    fuse::io::VirtualFileSystem::instance().mount(fuse::io::MountKind::Game, ".", "/game");
    fuse::log::info("vfs mounts: %u", fuse::io::VirtualFileSystem::instance().mountCount());

    runJobSmoke();

    check(fuse::legacy::parallel_for_smoke_sum(0u, 100u, 10u) == 4950u,
          "legacy parallel_for adapter sum matches serial expectation");

    check(fuse::legacy::t3d::initialize(), "fuse_t3d_legacy initialize");
    check(fuse::legacy::t2d::initialize(), "fuse_t2d_legacy initialize");

    check(fuse::legacy::t3d::isInitialized(), "t3d legacy running");
    check(fuse::legacy::t2d::isInitialized(), "t2d legacy running");

    runLegacyConSmoke();

    fuse::legacy::t3d::Con::execute("echo T3D dimension alive");
    fuse::legacy::t2d::Con::execute("echo T2D dimension alive");

    check(fuse::legacy::t3d::stringTableEntryCount() >= 1u, "t3d string table populated");
    check(fuse::legacy::t2d::stringTableEntryCount() >= 1u, "t2d string table populated");

    runStringInternSmoke();
    runSimObjectBridgeSmoke();
    runImageCompressSmoke();

#if defined(FUSE_T3D_LEGACY_ENGINE_PROBE)
    {
        fuse::u16 src[4] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
        fuse::u16 dst[1] = {};
        fuse::legacy::t3d::engineProbe::bitmapExtrude5551Smoke(src, dst, 2, 2);
        check(dst[0] != 0, "engine probe bitmapExtrude5551 produces non-zero mip");

        fuse::u8 rgb[6] = {255, 0, 0, 0, 255, 0};
        fuse::legacy::t3d::engineProbe::bitmapConvertRGB5551Smoke(rgb, 2);
        check(rgb[0] != 255 || rgb[1] != 0, "engine probe bitmapConvertRGB_to_5551 mutates pixels");

        const float halfOne = fuse::legacy::t3d::engineProbe::convertHalfFloatSmoke(0x3C00u);
        check(halfOne > 0.9f && halfOne < 1.1f, "engine probe convertHalfToFloat near 1.0");

        check(fuse::legacy::t3d::engineProbe::iesLoadEmptySmoke(),
              "engine probe ies_loader rejects empty input");
        const fuse::u32 md5Sum = fuse::legacy::t3d::engineProbe::md5DigestSmoke("fuse_u2_probe");
        check(md5Sum != 0u, "engine probe md5 digest non-zero");

        const fuse::u32 hash32 = fuse::legacy::t3d::engineProbe::hash32Smoke("fuse_u2_probe");
        check(hash32 != 0u, "engine probe hash32 non-zero");
        const fuse::u64 hash64 = fuse::legacy::t3d::engineProbe::hash64Smoke("fuse_u2_probe");
        check(hash64 != 0u, "engine probe hash64 non-zero");
        const char* stringHash64 = fuse::legacy::t3d::engineProbe::stringHash64Smoke("line\nbreak");
        check(stringHash64 != nullptr && stringHash64[0] != '\0', "engine probe getStringHash64 non-empty");
        check(fuse::legacy::t3d::engineProbe::swizzleBgraSmoke(), "engine probe Swizzles::bgra ToBuffer");
        check(fuse::legacy::t3d::engineProbe::fourccSmoke() == 0x45535546u,
              "engine probe MakeFourCC FUSE tag");
        check(fuse::legacy::t3d::engineProbe::memStreamRoundTripSmoke(),
              "engine probe MemStream write/read round-trip");
        check(fuse::legacy::t3d::engineProbe::fileStreamTempRoundTripSmoke(),
              "engine probe FileStream temp file round-trip");
        check(fuse::legacy::t3d::engineProbe::bitmapStbMemoryLoadSmoke(),
              "engine probe bitmapSTB MemStream 1x1 BMP load via readBitmapStream");
        check(fuse::legacy::t3d::engineProbe::readBitmapRejectsUnknownSmoke(),
              "engine probe readBitmapStream rejects unknown format");
        check(fuse::legacy::t3d::engineProbe::readBitmapPathSmoke(),
              "engine probe readBitmap path dispatch loads 1x1 BMP");
        check(fuse::legacy::t3d::engineProbe::writeBitmapRejectsUnknownSmoke(),
              "engine probe writeBitmapStream rejects unknown format");
        check(fuse::legacy::t3d::engineProbe::writeBitmapStreamRoundTripSmoke(),
              "engine probe writeBitmapStream STB TGA encode");
        check(fuse::legacy::t3d::engineProbe::writeBitmapPathSmoke(),
#if defined(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG)
              "engine probe writeBitmap PNG path round-trip");
#else
              "engine probe writeBitmap STB BMP path round-trip");
#endif
#if defined(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG)
        check(fuse::legacy::t3d::engineProbe::writeBitmapPngRoundTripSmoke(),
              "engine probe writeBitmapStream PNG round-trip via bitmapPng");
#endif
        check(fuse::legacy::t3d::engineProbe::timeClassSmoke(),
              "engine probe Torque::Time date round-trip");
        check(fuse::legacy::t3d::engineProbe::signalSmoke(),
              "engine probe Signal<void> notify/trigger");
        check(fuse::legacy::t3d::engineProbe::crcSmoke(),
              "engine probe CRC::calculateCRC non-trivial digest");
        check(fuse::legacy::t3d::engineProbe::idGeneratorSmoke(),
              "engine probe IdGenerator allocate/free/reuse");
        check(fuse::legacy::t3d::engineProbe::bitVectorSmoke(),
              "engine probe BitVector set/test");
        check(fuse::legacy::t3d::engineProbe::colorStaticConstSmoke(),
              "engine probe ColorI/LinearColorF static constants");
        check(fuse::legacy::t3d::engineProbe::stockColorSmoke(),
              "engine probe StockColor create/isColor/colorI");
        check(fuse::legacy::t3d::engineProbe::dataChunkerSmoke(),
              "engine probe DataChunker alloc/isManagedByChunker");
        check(fuse::legacy::t3d::engineProbe::resizeFilterStreamSmoke(),
              "engine probe ResizeFilterStream offset window read");
        check(fuse::legacy::t3d::engineProbe::tagDictionarySmoke(),
              "engine probe TagDictionary addEntry/defineToId round-trip");
        check(fuse::legacy::t3d::engineProbe::findMatchSmoke(),
              "engine probe FindMatch wildcard isMatch/findMatch");
        check(fuse::legacy::t3d::engineProbe::tokenizerSmoke(),
              "engine probe Tokenizer setBuffer/getNextToken");
        check(fuse::legacy::t3d::engineProbe::rgb2xyzSmoke(),
              "engine probe ConvertRGB toXYZ/fromXYZ round-trip");
        check(fuse::legacy::t3d::engineProbe::rgb2luvSmoke(),
              "engine probe ConvertRGB toLUV/toLUVScaled");
        check(fuse::legacy::t3d::engineProbe::bitStreamRoundTripSmoke(),
              "engine probe BitStream U32 write/read round-trip");
        check(fuse::legacy::t3d::engineProbe::bitStreamClassIdSmoke(),
              "engine probe BitStream writeClassId/readClassId round-trip");
        check(fuse::legacy::t3d::engineProbe::bitStreamHuffmanStringSmoke(),
              "engine probe BitStream Huffman writeString/readString round-trip");
        check(fuse::legacy::t3d::engineProbe::stringBufferUtf8Smoke(),
              "engine probe StringBuffer UTF-8 set/append/getUTF8");
        check(fuse::legacy::t3d::engineProbe::stringStartsEndsSmoke(),
              "engine probe String startsWith/endsWith prefix/suffix");
        check(fuse::legacy::t3d::engineProbe::uuidRoundTripSmoke(),
              "engine probe UUID generate/toString/fromString round-trip");
        check(fuse::legacy::t3d::engineProbe::gbitmapTransparencySmoke(),
              "engine probe GBitmap checkForTransparency alpha scan");
        check(fuse::legacy::t3d::engineProbe::gbitmapFillWhiteSmoke(),
              "engine probe GBitmap fillWhite/getSurfaceSize");
        check(fuse::legacy::t3d::engineProbe::gbitmapExtensionListSmoke(),
              "engine probe GBitmap sGetExtensionList includes STB bmp");
        check(fuse::legacy::t3d::engineProbe::gbitmapColorRgba8Smoke(),
              "engine probe GBitmap getColor/setColor RGBA8");
        check(fuse::legacy::t3d::engineProbe::gbitmapColorRgb8Smoke(),
              "engine probe GBitmap getColor/setColor RGB8");
        check(fuse::legacy::t3d::engineProbe::gbitmapExtrudeMipLevelsSmoke(),
              "engine probe GBitmap allocateBitmap extrudeMipLevels mip chain");
    }
#endif

#if defined(FUSE_HAS_VULKAN_RHI)
    {
        fuse::renderer::RendererBootstrapDesc rendererDesc{};
        rendererDesc.rhi.bootstrap.instance.enableValidation = false;
        auto rendererBootstrap = fuse::renderer::RendererBootstrap::create(rendererDesc);
        check(rendererBootstrap != nullptr, "RendererBootstrap allocated in smoke process");
        check(rendererBootstrap->isReady(), "RendererBootstrap init/shutdown path OK");
        rendererBootstrap->shutdown();
    }
#endif

    fuse::legacy::t2d::shutdown();
    fuse::legacy::t3d::shutdown();
    fuse::core::shutdown();

    if (g_failures == 0) {
        fuse::log::info("fuse_runtime_smoke: PASS — core + both prefixed legacy libs in one process");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_runtime_smoke: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
