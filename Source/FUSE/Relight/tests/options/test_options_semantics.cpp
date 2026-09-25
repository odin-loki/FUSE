/*
* Copyright (c.get()) 2022-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c.get()) 2026 FUSE contributors (MIT)
// rl_options / semantics: port of dxvk-remix tests/rtx/unit/test_rtx_option.cpp@0867d3c (MIT,
// Copyright (c.get()) NVIDIA CORPORATION), rewritten for the FUSE option API, plus Relight additions
// (routing, aliases, blocking layers, redundancy, migration helpers, malformed-value parity).

#include "rl_options_test.hpp"

#include <fuse/core/temp_path.hpp>
#include <fuse/relight/options/options.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace rl_options_test {
namespace {

using namespace fuse::relight::options;

int s_intCallbackCount = 0;
int s_floatCallbackCount = 0;
int s_chainCallbackCount = 0;
int s_cycleCallbackCount = 0;

void testIntOnChangeCallback(void*) { ++s_intCallbackCount; }
void testFloatOnChangeCallback(void*) { ++s_floatCallbackCount; }
void testRangeMinOnChangeCallback(void*);
void testRangeMaxOnChangeCallback(void*);
void testChainedBoundsCallback(void*);
void testCyclicBoundsACallback(void*);
void testCyclicBoundsBCallback(void*);
void testValueChainACallback(void*);
void testValueChainBCallback(void*);
void testValueChainCCallback(void*);
void testValueChainDCallback(void*);
void testValueCycleACallback(void*);
void testValueCycleBCallback(void*);

const OptionLayerKey kTestLayerMidKey{10000, "TestLayerMid"};
const OptionLayerKey kTestLayerHighKey{20000, "TestLayerHigh"};

struct TestOptions {
    // Basic types
    FUSE_RELIGHT_OPTION("rtx.test", bool, testBool, false, "Test boolean option");
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testInt, 100, "Test integer option");
    FUSE_RELIGHT_OPTION("rtx.test", float, testFloat, 1.5f, "Test float option");
    FUSE_RELIGHT_OPTION("rtx.test", std::string, testString, "default", "Test string option");
    // Vectors
    FUSE_RELIGHT_OPTION("rtx.test", Vec2f, testVector2, Vec2f(1.0f, 2.0f), "Test Vector2 option");
    FUSE_RELIGHT_OPTION("rtx.test", Vec3f, testVector3, Vec3f(1.0f, 2.0f, 3.0f), "Test Vector3 option");
    FUSE_RELIGHT_OPTION("rtx.test", Vec4f, testVector4, Vec4f(1.0f, 2.0f, 3.0f, 4.0f), "Test Vector4 option");
    FUSE_RELIGHT_OPTION("rtx.test", Vec2i, testVector2i, Vec2i(10, 20), "Test Vector2i option");
    // Hash collections
    FUSE_RELIGHT_OPTION("rtx.test", HashSet, testHashSet, {}, "Test hash set option");
    FUSE_RELIGHT_OPTION("rtx.test", HashVector, testHashVector, {}, "Test hash vector option");
    // Min/max arguments
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testIntWithMin, 50, "Test int with min value",
                             args.minValue = 0);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testIntWithMax, 50, "Test int with max value",
                             args.maxValue = 100);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testIntWithMinMax, 50, "Test int with min and max",
                             args.minValue = 0;
                             args.maxValue = 100);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testFloatWithMinMax, 0.5f, "Test float with min and max",
                             args.minValue = 0.0f;
                             args.maxValue = 1.0f);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", Vec2f, testVector2WithMinMax, Vec2f(0.5f, 0.5f),
                             "Test Vector2 with min and max",
                             args.minValue = Vec2f(0.0f, 0.0f);
                             args.maxValue = Vec2f(1.0f, 1.0f));
    // Callbacks
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testIntWithCallback, 0, "Test int with onChange callback",
                             args.onChangeCallback = testIntOnChangeCallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testFloatWithCallback, 0.0f,
                             "Test float with onChange callback for blending",
                             args.onChangeCallback = testFloatOnChangeCallback);
    // Environment variables
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testIntWithEnv, 123, "Test int with environment variable",
                             args.environment = "RTX_TEST_INT_ENV");
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", bool, testBoolWithEnv, false, "Test bool with environment variable",
                             args.environment = "RTX_TEST_BOOL_ENV");
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testFloatWithEnv, 1.5f, "Test float with environment variable",
                             args.environment = "RTX_TEST_FLOAT_ENV");
    // Flags
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testIntNoSave, 42, "Test int with NoSave flag",
                             args.flags = OptionFlags::NoSave);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testIntNoReset, 42, "Test int with NoReset flag",
                             args.flags = OptionFlags::NoReset);
    FUSE_RELIGHT_OPTION_FLAG_ENV("rtx.test", std::int32_t, testIntEnvAndFlags, 99, OptionFlags::NoSave,
                                 "RTX_TEST_INT_ENV_FLAGS", "Test int with env and NoSave flag");
    // Invalidation scope
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testScopeRead, 1, "Test int read inside an invalidation scope");
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testScopeUnread, 1, "Test int never read");
    FUSE_RELIGHT_OPTION("rtx.test", HashSet, testScopeHashSet, {}, "Test hash set for containsHash() tagging");
    // Enum (stored as int)
    enum class TestEnum { ValueA = 0, ValueB = 1, ValueC = 2 };
    FUSE_RELIGHT_OPTION("rtx.test", TestEnum, testEnum, TestEnum::ValueA, "Test enum option");
    // Dedicated options for layer tests
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testIntLayerPriority, 100, "Test int for layer priority");
    FUSE_RELIGHT_OPTION("rtx.test", float, testFloatBlend, 1.5f, "Test float for blending");
    FUSE_RELIGHT_OPTION("rtx.test", Vec3f, testVector3Blend, Vec3f(1.0f, 2.0f, 3.0f), "Test Vector3 for blending");
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testIntEnableDisable, 100, "Test int for enable/disable layer test");
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testIntThreshold, 100, "Test int for threshold test");
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testIntComplex, 100, "Test int for complex layer test");
    // Min/max interdependency
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testRangeMin, 0.0f, "Test min value of a range",
                             args.minValue = -100.0f;
                             args.maxValue = 100.0f;
                             args.onChangeCallback = testRangeMinOnChangeCallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testRangeMax, 10.0f, "Test max value of a range",
                             args.minValue = -100.0f;
                             args.maxValue = 100.0f;
                             args.onChangeCallback = testRangeMaxOnChangeCallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testChainedSource, 50.0f, "Source option that sets bounds on target",
                             args.minValue = 0.0f;
                             args.maxValue = 100.0f;
                             args.onChangeCallback = testChainedBoundsCallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testChainedTarget, 50.0f, "Target option with dynamic bounds",
                             args.minValue = 0.0f;
                             args.maxValue = 100.0f);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testCyclicA, 50.0f, "Cyclic option A that adjusts B's bounds",
                             args.minValue = 0.0f;
                             args.maxValue = 100.0f;
                             args.onChangeCallback = testCyclicBoundsACallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", float, testCyclicB, 50.0f, "Cyclic option B that adjusts A's bounds",
                             args.minValue = 0.0f;
                             args.maxValue = 100.0f;
                             args.onChangeCallback = testCyclicBoundsBCallback);
    // Value-setting chain A -> B -> C -> D and cycle A <-> B
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testValueChainA, 0, "Value chain A",
                             args.onChangeCallback = testValueChainACallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testValueChainB, 0, "Value chain B",
                             args.onChangeCallback = testValueChainBCallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testValueChainC, 0, "Value chain C",
                             args.onChangeCallback = testValueChainCCallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testValueChainD, 0, "Value chain D (end of chain)",
                             args.onChangeCallback = testValueChainDCallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testValueCycleA, 0, "Value cycle A",
                             args.onChangeCallback = testValueCycleACallback);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testValueCycleB, 0, "Value cycle B",
                             args.onChangeCallback = testValueCycleBCallback);
    // Migration
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testMigrateDeveloper, 100, "Developer option for migration test");
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testMigrateUser, 200, "User option for migration test",
                             args.flags = OptionFlags::UserSetting);
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", std::int32_t, testMigrateUserNoReset, 300,
                             "User option with NoReset for migration test",
                             args.flags = OptionFlags::UserSetting | OptionFlags::NoReset);
    FUSE_RELIGHT_OPTION("rtx.test", HashSet, testMigrateDeveloperHash, {}, "Developer hashset for migration test");
    FUSE_RELIGHT_OPTION_ARGS("rtx.test", HashSet, testMigrateUserHash, {}, "User hashset for migration test",
                             args.flags = OptionFlags::UserSetting);
    // Relight additions
    FUSE_RELIGHT_OPTION_FLAG("rtx.test", std::int32_t, testIntDrawcall, 0, OptionFlags::InvalidatesDrawcallTranslation,
                             "Test int that invalidates draw call translation");
    FUSE_RELIGHT_OPTION("rtx.test", std::int32_t, testMigrateSource, 5, "Source of migrateValuesTo");
    FUSE_RELIGHT_OPTION("rtx.test", float, testMigrateDestination, 0.0f, "Destination of migrateValuesTo");
    FUSE_RELIGHT_OPTION("relight.test", std::int32_t, nativeOnly, 7, "FUSE-only option in the relight namespace");
};

void testRangeMinOnChangeCallback(void*) {
    TestOptions::testRangeMaxObject().setMinValue(TestOptions::testRangeMin());
}
void testRangeMaxOnChangeCallback(void*) {
    TestOptions::testRangeMinObject().setMaxValue(TestOptions::testRangeMax());
}
void testChainedBoundsCallback(void*) {
    ++s_chainCallbackCount;
    TestOptions::testChainedTargetObject().setMaxValue(TestOptions::testChainedSource());
}
void testCyclicBoundsACallback(void*) {
    ++s_cycleCallbackCount;
    TestOptions::testCyclicBObject().setMinValue(TestOptions::testCyclicA());
}
void testCyclicBoundsBCallback(void*) {
    ++s_cycleCallbackCount;
    TestOptions::testCyclicAObject().setMinValue(TestOptions::testCyclicB());
}
void testValueChainACallback(void*) {
    ++s_chainCallbackCount;
    TestOptions::testValueChainB.setDeferred(TestOptions::testValueChainA() + 1);
}
void testValueChainBCallback(void*) {
    ++s_chainCallbackCount;
    TestOptions::testValueChainC.setDeferred(TestOptions::testValueChainB() + 1);
}
void testValueChainCCallback(void*) {
    ++s_chainCallbackCount;
    TestOptions::testValueChainD.setDeferred(TestOptions::testValueChainC() + 1);
}
void testValueChainDCallback(void*) { ++s_chainCallbackCount; }
void testValueCycleACallback(void*) {
    ++s_cycleCallbackCount;
    TestOptions::testValueCycleB.setDeferred(TestOptions::testValueCycleA() + 1);
}
void testValueCycleBCallback(void*) {
    ++s_cycleCallbackCount;
    TestOptions::testValueCycleA.setDeferred(TestOptions::testValueCycleB() + 1);
}

void apply() { OptionManager::applyPendingValues(nullptr, false); }

OptionLayerHandle acquire(const OptionLayerKey& key, float strength = 1.0f, float threshold = 0.1f) {
    static const OptionConfig kEmptyConfig;
    return OptionManager::acquireLayer("", key, strength, threshold, false, &kEmptyConfig);
}

/// Called at the end of tests: nothing may leak after the test layers are released.
void verifyOptionsAtDefaults() {
    RL_CHECK(TestOptions::testBool() == false);
    RL_CHECK(TestOptions::testInt() == 100);
    RL_CHECK_NEAR(TestOptions::testFloat(), 1.5f, 0.0001f);
    RL_CHECK_STR(TestOptions::testString(), "default");
    RL_CHECK(TestOptions::testVector2() == Vec2f(1.0f, 2.0f));
    RL_CHECK(TestOptions::testVector3() == Vec3f(1.0f, 2.0f, 3.0f));
    RL_CHECK(TestOptions::testIntLayerPriority() == 100);
    RL_CHECK(TestOptions::testIntEnableDisable() == 100);
    RL_CHECK(TestOptions::testIntThreshold() == 100);
    RL_CHECK(TestOptions::testIntComplex() == 100);
    RL_CHECK_NEAR(TestOptions::testFloatBlend(), 1.5f, 0.0001f);
    RL_CHECK(TestOptions::testVector3Blend() == Vec3f(1.0f, 2.0f, 3.0f));
    RL_CHECK(TestOptions::testIntWithCallback() == 0);
    RL_CHECK_NEAR(TestOptions::testFloatWithCallback(), 0.0f, 0.0001f);
    RL_CHECK(TestOptions::testHashSet().empty());
}

std::filesystem::path s_tempDir;

// ============================================================================
// Ported tests
// ============================================================================

void test_basicTypes() {
    RL_CHECK(TestOptions::testBool() == false);
    RL_CHECK(TestOptions::testInt() == 100);
    RL_CHECK_NEAR(TestOptions::testFloat(), 1.5f, 0.0001f);
    RL_CHECK_STR(TestOptions::testString(), "default");
    RL_CHECK(TestOptions::testVector2() == Vec2f(1.0f, 2.0f));
    RL_CHECK(TestOptions::testVector3() == Vec3f(1.0f, 2.0f, 3.0f));
    RL_CHECK(TestOptions::testVector4() == Vec4f(1.0f, 2.0f, 3.0f, 4.0f));
    RL_CHECK(TestOptions::testVector2i() == Vec2i(10, 20));
    RL_CHECK(TestOptions::testEnum() == TestOptions::TestEnum::ValueA);
    RL_CHECK(TestOptions::testHashSet().empty());
    RL_CHECK(TestOptions::testHashVector().empty());
}

void test_setAndGet() {
    OptionLayerHandle layer = acquire(kTestLayerMidKey);
    RL_CHECK(layer.get() != nullptr);
    TestOptions::testBool.setDeferred(true, layer.get());
    apply();
    RL_CHECK(TestOptions::testBool() == true);
    TestOptions::testInt.setDeferred(200, layer.get());
    apply();
    RL_CHECK(TestOptions::testInt() == 200);
    TestOptions::testFloat.setDeferred(3.14f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testFloat(), 3.14f, 0.0001f);
    TestOptions::testString.setDeferred("modified", layer.get());
    apply();
    RL_CHECK_STR(TestOptions::testString(), "modified");
    TestOptions::testVector2.setDeferred(Vec2f(5.0f, 6.0f), layer.get());
    apply();
    RL_CHECK(TestOptions::testVector2() == Vec2f(5.0f, 6.0f));
    TestOptions::testVector3.setDeferred(Vec3f(7.0f, 8.0f, 9.0f), layer.get());
    apply();
    RL_CHECK(TestOptions::testVector3() == Vec3f(7.0f, 8.0f, 9.0f));
    TestOptions::testEnum.setDeferred(TestOptions::TestEnum::ValueB, layer.get());
    apply();
    RL_CHECK(TestOptions::testEnum() == TestOptions::TestEnum::ValueB);
    // Deferred means deferred: the new value is not visible until the end of the frame.
    TestOptions::testInt.setDeferred(300, layer.get());
    RL_CHECK(TestOptions::testInt() == 200);
    apply();
    RL_CHECK(TestOptions::testInt() == 300);
    layer.release();
    apply();
    RL_CHECK(TestOptions::testEnum() == TestOptions::TestEnum::ValueA);
    verifyOptionsAtDefaults();
}

void test_getDefaultValue() {
    RL_CHECK(TestOptions::testBool.getDefaultValue() == false);
    RL_CHECK(TestOptions::testInt.getDefaultValue() == 100);
    RL_CHECK_NEAR(TestOptions::testFloat.getDefaultValue(), 1.5f, 0.0001f);
    RL_CHECK_STR(TestOptions::testString.getDefaultValue(), "default");
    RL_CHECK(TestOptions::testVector2.getDefaultValue() == Vec2f(1.0f, 2.0f));
}

void test_fullOptionName() {
    RL_CHECK_STR(TestOptions::testIntObject().getFullName(), "rtx.test.testInt");
    RL_CHECK_STR(TestOptions::testBoolObject().getFullName(), "rtx.test.testBool");
    RL_CHECK_STR(TestOptions::nativeOnlyObject().getFullName(), "relight.test.nativeOnly");
}

void test_optionTypeIdentification() {
    RL_CHECK(TestOptions::testBoolObject().getType() == OptionType::Bool);
    RL_CHECK(TestOptions::testIntObject().getType() == OptionType::Int);
    RL_CHECK(TestOptions::testFloatObject().getType() == OptionType::Float);
    RL_CHECK(TestOptions::testStringObject().getType() == OptionType::String);
    RL_CHECK(TestOptions::testVector2Object().getType() == OptionType::Vector2);
    RL_CHECK(TestOptions::testVector3Object().getType() == OptionType::Vector3);
    RL_CHECK(TestOptions::testVector4Object().getType() == OptionType::Vector4);
    RL_CHECK(TestOptions::testVector2iObject().getType() == OptionType::Vector2i);
    RL_CHECK(TestOptions::testHashSetObject().getType() == OptionType::HashSet);
    RL_CHECK(TestOptions::testHashVectorObject().getType() == OptionType::HashVector);
    RL_CHECK(TestOptions::testEnumObject().getType() == OptionType::Int); // enums are stored as int
}

void test_minMaxClamping() {
    OptionLayerHandle layer = acquire(kTestLayerHighKey);
    TestOptions::testIntWithMin.setDeferred(-10, layer.get());
    apply();
    RL_CHECK(TestOptions::testIntWithMin() == 0);
    TestOptions::testIntWithMax.setDeferred(200, layer.get());
    apply();
    RL_CHECK(TestOptions::testIntWithMax() == 100);
    TestOptions::testIntWithMinMax.setDeferred(-50, layer.get());
    apply();
    RL_CHECK(TestOptions::testIntWithMinMax() == 0);
    TestOptions::testIntWithMinMax.setDeferred(150, layer.get());
    apply();
    RL_CHECK(TestOptions::testIntWithMinMax() == 100);
    TestOptions::testIntWithMinMax.setDeferred(75, layer.get());
    apply();
    RL_CHECK(TestOptions::testIntWithMinMax() == 75);
    TestOptions::testFloatWithMinMax.setDeferred(-0.5f, layer.get());
    apply();
    RL_CHECK(TestOptions::testFloatWithMinMax() == 0.0f);
    TestOptions::testFloatWithMinMax.setDeferred(1.5f, layer.get());
    apply();
    RL_CHECK(TestOptions::testFloatWithMinMax() == 1.0f);
    TestOptions::testVector2WithMinMax.setDeferred(Vec2f(-0.5f, 0.5f), layer.get());
    apply();
    RL_CHECK(TestOptions::testVector2WithMinMax() == Vec2f(0.0f, 0.5f));
    TestOptions::testVector2WithMinMax.setDeferred(Vec2f(0.5f, 1.5f), layer.get());
    apply();
    RL_CHECK(TestOptions::testVector2WithMinMax() == Vec2f(0.5f, 1.0f));
    // The layer keeps the raw value; only the resolved value is clamped.
    const std::optional<OptionValue> raw = TestOptions::testIntWithMinMaxObject().getLayerValue(layer.get());
    RL_CHECK(raw && std::get<std::int32_t>(*raw) == 75);
    RL_CHECK(TestOptions::testIntWithMinMax.getMinValue() == std::optional<std::int32_t>(0));
    RL_CHECK(TestOptions::testIntWithMinMax.getMaxValue() == std::optional<std::int32_t>(100));
    RL_CHECK(!TestOptions::testIntWithMin.getMaxValue().has_value());
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_dynamicMinMax() {
    OptionLayerHandle layer = acquire({10500, "DynamicMinMaxLayer"});
    TestOptions::testInt.setMinValue(50);
    TestOptions::testInt.setDeferred(25, layer.get());
    apply();
    RL_CHECK(TestOptions::testInt() == 50);
    TestOptions::testInt.setMaxValue(200);
    TestOptions::testInt.setDeferred(300, layer.get());
    apply();
    RL_CHECK(TestOptions::testInt() == 200);
    TestOptions::testInt.setDeferred(150, layer.get());
    apply();
    RL_CHECK(TestOptions::testInt() == 150);
    // Tightening a bound re-clamps the current value (setMinValue marks the option dirty).
    TestOptions::testInt.setMaxValue(120);
    apply();
    RL_CHECK(TestOptions::testInt() == 120);
    TestOptions::testInt.setMinValue(std::numeric_limits<std::int32_t>::min());
    TestOptions::testInt.setMaxValue(std::numeric_limits<std::int32_t>::max());
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_onChangeCallback() {
    // Startup (OptionSystem::initialize) runs every callback once.
    RL_CHECK(s_intCallbackCount == 1);
    RL_CHECK(s_floatCallbackCount == 1);
    RL_CHECK(TestOptions::testIntWithCallbackObject().getFlags() == 0);
    RL_CHECK(TestOptions::testFloatWithCallbackObject().getFlags() == 0);

    OptionLayerHandle lowLayer = acquire({13000, "CallbackTestLowLayer"});
    OptionLayerHandle highLayer = acquire({14000, "CallbackTestHighLayer"});
    int intBefore = s_intCallbackCount;
    int floatBefore = s_floatCallbackCount;

    TestOptions::testIntWithCallback.setDeferred(999, highLayer.get());
    apply();
    RL_CHECK(s_intCallbackCount == intBefore + 1);
    RL_CHECK(s_floatCallbackCount == floatBefore);
    RL_CHECK(TestOptions::testIntWithCallback() == 999);
    intBefore = s_intCallbackCount;

    TestOptions::testIntWithCallback.setDeferred(999, highLayer.get()); // same value: no callback
    apply();
    RL_CHECK(s_intCallbackCount == intBefore);

    TestOptions::testIntWithCallback.setDeferred(500, lowLayer.get()); // hidden by the high layer
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 999);
    RL_CHECK(s_intCallbackCount == intBefore);

    TestOptions::testIntWithCallbackObject().disableLayerValue(highLayer.get()); // falls back to 500
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 500);
    RL_CHECK(s_intCallbackCount == intBefore + 1);
    intBefore = s_intCallbackCount;

    lowLayer.release();
    highLayer.release();
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 0);
    RL_CHECK(s_intCallbackCount == intBefore + 1);
    intBefore = s_intCallbackCount;
    floatBefore = s_floatCallbackCount;

    // Float blending: 100 * 0.5 + 0 * 0.5.
    OptionLayerHandle blendLayer = acquire({15000, "CallbackBlendLayer"}, 0.5f);
    TestOptions::testFloatWithCallback.setDeferred(100.0f, blendLayer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testFloatWithCallback(), 50.0f, 0.01f);
    RL_CHECK(s_floatCallbackCount == floatBefore + 1);
    RL_CHECK(s_intCallbackCount == intBefore);
    floatBefore = s_floatCallbackCount;

    blendLayer->requestBlendStrength(1.0f);
    apply();
    RL_CHECK_NEAR(TestOptions::testFloatWithCallback(), 100.0f, 0.01f);
    RL_CHECK(s_floatCallbackCount == floatBefore + 1);
    floatBefore = s_floatCallbackCount;

    blendLayer->requestBlendStrength(1.0f); // no change
    apply();
    RL_CHECK(s_floatCallbackCount == floatBefore);

    blendLayer->requestBlendStrength(0.5f);
    apply();
    RL_CHECK_NEAR(TestOptions::testFloatWithCallback(), 50.0f, 0.01f);
    RL_CHECK(s_floatCallbackCount == floatBefore + 1);
    floatBefore = s_floatCallbackCount;

    blendLayer.release();
    apply();
    RL_CHECK_NEAR(TestOptions::testFloatWithCallback(), 0.0f, 0.01f);
    RL_CHECK(s_floatCallbackCount == floatBefore + 1);

    // Ints do not blend: they apply only while strength >= threshold.
    OptionLayerHandle thresholdLayer = acquire({16000, "ThresholdTestLayer"}, 0.3f, 0.5f);
    TestOptions::testIntWithCallback.setDeferred(777, thresholdLayer.get());
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 0);
    RL_CHECK(s_intCallbackCount == intBefore);

    thresholdLayer->requestBlendStrength(0.6f);
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 777);
    RL_CHECK(s_intCallbackCount == intBefore + 1);
    intBefore = s_intCallbackCount;

    thresholdLayer->requestBlendStrength(0.8f);
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 777);
    RL_CHECK(s_intCallbackCount == intBefore);

    thresholdLayer->requestBlendStrength(0.4f);
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 0);
    RL_CHECK(s_intCallbackCount == intBefore + 1);
    intBefore = s_intCallbackCount;

    thresholdLayer->requestBlendThreshold(0.3f);
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 777);
    RL_CHECK(s_intCallbackCount == intBefore + 1);
    intBefore = s_intCallbackCount;

    thresholdLayer.release();
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 0);
    RL_CHECK(s_intCallbackCount == intBefore + 1);

    verifyOptionsAtDefaults();
    s_intCallbackCount = 0;
    s_floatCallbackCount = 0;
}

void test_minMaxInterdependency() {
    TestOptions::testRangeMinObject().setMinValue(-100.0f);
    TestOptions::testRangeMinObject().setMaxValue(100.0f);
    TestOptions::testRangeMaxObject().setMinValue(-100.0f);
    TestOptions::testRangeMaxObject().setMaxValue(100.0f);
    OptionLayerHandle layer = acquire({30000, "MinMaxTestLayer"});

    TestOptions::testRangeMax.setDeferred(50.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testRangeMax(), 50.0f, 0.001f);
    TestOptions::testRangeMin.setDeferred(60.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testRangeMin(), 50.0f, 0.001f);

    TestOptions::testRangeMin.setDeferred(20.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testRangeMin(), 20.0f, 0.001f);
    TestOptions::testRangeMax.setDeferred(10.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testRangeMax(), 20.0f, 0.001f);

    TestOptions::testRangeMin.setDeferred(5.0f, layer.get());
    TestOptions::testRangeMax.setDeferred(95.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testRangeMin(), 5.0f, 0.001f);
    RL_CHECK_NEAR(TestOptions::testRangeMax(), 95.0f, 0.001f);

    layer.release();
    apply();
    TestOptions::testRangeMinObject().setMinValue(-100.0f);
    TestOptions::testRangeMinObject().setMaxValue(100.0f);
    TestOptions::testRangeMaxObject().setMinValue(-100.0f);
    TestOptions::testRangeMaxObject().setMaxValue(100.0f);
    apply();
}

void test_chainedOnChangeCallbacks() {
    TestOptions::testChainedSourceObject().setMinValue(0.0f);
    TestOptions::testChainedSourceObject().setMaxValue(100.0f);
    TestOptions::testChainedTargetObject().setMinValue(0.0f);
    TestOptions::testChainedTargetObject().setMaxValue(100.0f);
    OptionLayerHandle layer = acquire({33000, "ChainedBoundsTestLayer"});
    s_chainCallbackCount = 0;

    TestOptions::testChainedSource.setDeferred(30.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testChainedSource(), 30.0f, 0.001f);
    RL_CHECK(s_chainCallbackCount >= 1);
    TestOptions::testChainedTarget.setDeferred(50.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testChainedTarget(), 30.0f, 0.001f);

    s_chainCallbackCount = 0;
    TestOptions::testChainedSource.setDeferred(20.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testChainedSource(), 20.0f, 0.001f);
    RL_CHECK(s_chainCallbackCount >= 1);
    // The callback lowered the target's max, which re-resolves the target in the same call.
    RL_CHECK_NEAR(TestOptions::testChainedTarget(), 20.0f, 0.001f);

    TestOptions::testChainedTarget.setDeferred(15.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testChainedTarget(), 15.0f, 0.001f);

    layer.release();
    apply();
    TestOptions::testChainedSourceObject().setMinValue(0.0f);
    TestOptions::testChainedSourceObject().setMaxValue(100.0f);
    TestOptions::testChainedTargetObject().setMinValue(0.0f);
    TestOptions::testChainedTargetObject().setMaxValue(100.0f);
    apply();
    s_chainCallbackCount = 0;
}

void test_cyclicOnChangeCallbacksTerminate() {
    TestOptions::testCyclicAObject().setMinValue(0.0f);
    TestOptions::testCyclicAObject().setMaxValue(100.0f);
    TestOptions::testCyclicBObject().setMinValue(0.0f);
    TestOptions::testCyclicBObject().setMaxValue(100.0f);
    apply();
    OptionLayerHandle layer = acquire({34000, "CyclicBoundsTestLayer"});
    s_cycleCallbackCount = 0;

    TestOptions::testCyclicA.setDeferred(30.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testCyclicA(), 30.0f, 0.001f);
    RL_CHECK(s_cycleCallbackCount <= 8);
    RL_CHECK(s_cycleCallbackCount >= 1);
    const int countAfter = s_cycleCallbackCount;
    const float a = TestOptions::testCyclicA();
    const float b = TestOptions::testCyclicB();
    apply(); // dirty options do not persist across frames
    RL_CHECK(s_cycleCallbackCount == countAfter);
    RL_CHECK(TestOptions::testCyclicA() == a);
    RL_CHECK(TestOptions::testCyclicB() == b);

    layer.release();
    apply();
    TestOptions::testCyclicAObject().setMinValue(0.0f);
    TestOptions::testCyclicAObject().setMaxValue(100.0f);
    TestOptions::testCyclicBObject().setMinValue(0.0f);
    TestOptions::testCyclicBObject().setMaxValue(100.0f);
    apply();
    s_cycleCallbackCount = 0;
}

void test_valueSettingChain() {
    const OptionLayer* derived = OptionLayer::getDerivedLayer();
    RL_CHECK(derived != nullptr);
    s_chainCallbackCount = 0;
    TestOptions::testValueChainA.setDeferred(100, derived);
    apply();
    RL_CHECK(TestOptions::testValueChainA() == 100);
    RL_CHECK(TestOptions::testValueChainB() == 101);
    RL_CHECK(TestOptions::testValueChainC() == 102);
    RL_CHECK(TestOptions::testValueChainD() == 103);
    RL_CHECK(s_chainCallbackCount == 4);

    s_chainCallbackCount = 0;
    TestOptions::testValueChainA.setDeferred(200, derived);
    apply();
    RL_CHECK(TestOptions::testValueChainB() == 201);
    RL_CHECK(TestOptions::testValueChainC() == 202);
    RL_CHECK(TestOptions::testValueChainD() == 203);
    RL_CHECK(s_chainCallbackCount == 4);

    s_chainCallbackCount = 0;
    apply();
    RL_CHECK(s_chainCallbackCount == 0);
}

void test_cyclicValueSettingTerminates() {
    const OptionLayer* derived = OptionLayer::getDerivedLayer();
    s_cycleCallbackCount = 0;
    TestOptions::testValueCycleA.setDeferred(1000, derived);
    apply();
    RL_CHECK(TestOptions::testValueCycleA() >= 1000);
    RL_CHECK(TestOptions::testValueCycleB() >= 1000);
    RL_CHECK(s_cycleCallbackCount <= 8);
    RL_CHECK(s_cycleCallbackCount >= 2);
    const int countAfter = s_cycleCallbackCount;
    const std::int32_t a = TestOptions::testValueCycleA();
    const std::int32_t b = TestOptions::testValueCycleB();
    apply();
    RL_CHECK(s_cycleCallbackCount == countAfter);
    RL_CHECK(TestOptions::testValueCycleA() == a);
    RL_CHECK(TestOptions::testValueCycleB() == b);
    s_cycleCallbackCount = 0;
}

void test_environmentVariables() {
    const char* intEnv = TestOptions::testIntWithEnvObject().getEnvironmentVariable();
    RL_CHECK(intEnv && std::strcmp(intEnv, "RTX_TEST_INT_ENV") == 0);
    const char* boolEnv = TestOptions::testBoolWithEnvObject().getEnvironmentVariable();
    RL_CHECK(boolEnv && std::strcmp(boolEnv, "RTX_TEST_BOOL_ENV") == 0);
    const char* floatEnv = TestOptions::testFloatWithEnvObject().getEnvironmentVariable();
    RL_CHECK(floatEnv && std::strcmp(floatEnv, "RTX_TEST_FLOAT_ENV") == 0);
    const char* flagsEnv = TestOptions::testIntEnvAndFlagsObject().getEnvironmentVariable();
    RL_CHECK(flagsEnv && std::strcmp(flagsEnv, "RTX_TEST_INT_ENV_FLAGS") == 0);
    RL_CHECK((TestOptions::testIntEnvAndFlagsObject().getFlags() & OptionFlags::NoSave) != 0);
    const char* noEnv = TestOptions::testIntObject().getEnvironmentVariable();
    RL_CHECK(noEnv == nullptr || std::strlen(noEnv) == 0);

    OptionLayerHandle envTestLayer = acquire({25000, "EnvVarTestLayer"});
    RL_CHECK(setEnvironmentVariable("RTX_TEST_INT_ENV", "999"));
    RL_CHECK(setEnvironmentVariable("RTX_TEST_BOOL_ENV", "1"));
    RL_CHECK(setEnvironmentVariable("RTX_TEST_FLOAT_ENV", "3.14"));
    std::string outValue;
    RL_CHECK(TestOptions::testIntWithEnvObject().loadFromEnvironmentVariable(envTestLayer.get(), &outValue));
    RL_CHECK_STR(outValue, "999");
    RL_CHECK(TestOptions::testBoolWithEnvObject().loadFromEnvironmentVariable(envTestLayer.get(), &outValue));
    RL_CHECK_STR(outValue, "1");
    RL_CHECK(TestOptions::testFloatWithEnvObject().loadFromEnvironmentVariable(envTestLayer.get(), &outValue));
    RL_CHECK_STR(outValue, "3.14");
    apply();
    RL_CHECK(TestOptions::testIntWithEnv() == 999);
    RL_CHECK(TestOptions::testBoolWithEnv() == true);
    RL_CHECK_NEAR(TestOptions::testFloatWithEnv(), 3.14f, 0.001f);
    RL_CHECK(!TestOptions::testIntObject().loadFromEnvironmentVariable(envTestLayer.get(), nullptr));

    // A malformed environment value is reported (and not counted as loaded).
    RL_CHECK(setEnvironmentVariable("RTX_TEST_INT_ENV", "not-a-number"));
    RL_CHECK(!TestOptions::testIntWithEnvObject().loadFromEnvironmentVariable(envTestLayer.get(), nullptr));

    setEnvironmentVariable("RTX_TEST_INT_ENV", "");
    setEnvironmentVariable("RTX_TEST_BOOL_ENV", "");
    setEnvironmentVariable("RTX_TEST_FLOAT_ENV", "");
    envTestLayer.release();
    apply();
    RL_CHECK(TestOptions::testIntWithEnv() == 123);
    verifyOptionsAtDefaults();
}

void test_hashSetOperations() {
    const OptionLayerKey weakKey{10000, "WeakHashLayer"};
    const OptionLayerKey middleKey{15000, "MiddleHashLayer"};
    const OptionLayerKey strongKey{20000, "StrongHashLayer"};
    OptionLayerHandle weak = acquire(weakKey);
    OptionLayerHandle middle = acquire(middleKey);
    OptionLayerHandle strong = acquire(strongKey);
    const Hash64 hash1 = 0x1234567890ABCDEFull, hash2 = 0xFEDCBA0987654321ull, hash3 = 0xAAAABBBBCCCCDDDDull,
                 hash4 = 0x1111222233334444ull;

    TestOptions::testHashSet.addHash(hash1, weak.get());
    apply();
    RL_CHECK(TestOptions::testHashSet.containsHash(hash1));
    RL_CHECK(!TestOptions::testHashSet.containsHash(hash2));

    TestOptions::testHashSet.addHash(hash2, weak.get());
    TestOptions::testHashSet.removeHash(hash2, middle.get());
    apply();
    RL_CHECK(!TestOptions::testHashSet.containsHash(hash2));

    TestOptions::testHashSet.addHash(hash3, weak.get());
    TestOptions::testHashSet.removeHash(hash3, middle.get());
    TestOptions::testHashSet.addHash(hash3, strong.get());
    apply();
    RL_CHECK(TestOptions::testHashSet.containsHash(hash3));

    TestOptions::testHashSet.removeHash(hash4, weak.get());
    TestOptions::testHashSet.addHash(hash4, middle.get());
    apply();
    RL_CHECK(TestOptions::testHashSet.containsHash(hash4));
    // The resolved set exposes exactly the surviving positives.
    RL_CHECK((TestOptions::testHashSet() == HashSet{hash1, hash3, hash4}));

    strong.release();
    apply();
    RL_CHECK(!TestOptions::testHashSet.containsHash(hash3));
    RL_CHECK(TestOptions::testHashSet.containsHash(hash1));
    RL_CHECK(TestOptions::testHashSet.containsHash(hash4));

    middle.release();
    apply();
    RL_CHECK(TestOptions::testHashSet.containsHash(hash2));
    RL_CHECK(TestOptions::testHashSet.containsHash(hash3));
    RL_CHECK(!TestOptions::testHashSet.containsHash(hash4));

    OptionLayerHandle middle2 = acquire(middleKey);
    TestOptions::testHashSet.removeHash(hash1, middle2.get());
    apply();
    RL_CHECK(!TestOptions::testHashSet.containsHash(hash1));
    TestOptions::testHashSet.clearHash(hash1, middle2.get());
    apply();
    RL_CHECK(TestOptions::testHashSet.containsHash(hash1));
    // clearHash on the last opinion drops the layer entry altogether.
    RL_CHECK(!TestOptions::testHashSetObject().hasValueInLayer(middle2.get()));

    middle2.release();
    weak.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_hashSetLayerMerging() {
    OptionLayerHandle low = acquire({6000, "HashLowLayer"});
    OptionLayerHandle high = acquire({7000, "HashHighLayer"});
    const Hash64 a = 0x1111111111111111ull, b = 0x2222222222222222ull, c = 0x3333333333333333ull;
    TestOptions::testHashSet.addHash(a, low.get());
    TestOptions::testHashSet.addHash(b, low.get());
    TestOptions::testHashSet.addHash(c, low.get());
    apply();
    RL_CHECK(TestOptions::testHashSet.containsHash(a) && TestOptions::testHashSet.containsHash(b) &&
             TestOptions::testHashSet.containsHash(c));
    TestOptions::testHashSet.removeHash(b, high.get());
    apply();
    RL_CHECK(TestOptions::testHashSet.containsHash(a));
    RL_CHECK(!TestOptions::testHashSet.containsHash(b));
    RL_CHECK(TestOptions::testHashSet.containsHash(c));
    // Per-hash layer queries.
    RL_CHECK(TestOptions::testHashSetObject().hasValueInLayer(high.get(), b));
    RL_CHECK(!TestOptions::testHashSetObject().hasValueInLayer(high.get(), a));

    TestOptions::testHashSet.clearHash(a, low.get());
    TestOptions::testHashSet.clearHash(b, low.get());
    TestOptions::testHashSet.clearHash(c, low.get());
    TestOptions::testHashSet.clearHash(b, high.get());
    apply();
    low.release();
    high.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_layerPriorityOverride() {
    OptionLayerHandle low = acquire({16000, "PriorityTestLow"});
    OptionLayerHandle high = acquire({17000, "PriorityTestHigh"});
    TestOptions::testIntLayerPriority.setDeferred(500, low.get());
    apply();
    RL_CHECK(TestOptions::testIntLayerPriority() == 500);
    TestOptions::testIntLayerPriority.setDeferred(999, high.get());
    apply();
    RL_CHECK(TestOptions::testIntLayerPriority() == 999);
    TestOptions::testIntLayerPriorityObject().disableLayerValue(high.get());
    apply();
    RL_CHECK(TestOptions::testIntLayerPriority() == 500);
    low.release();
    high.release();
    apply();

    // Equal priority: the alphabetically earlier name wins.
    OptionLayerHandle aLayer = acquire({16500, "a.conf"});
    OptionLayerHandle zLayer = acquire({16500, "z.conf"});
    TestOptions::testIntLayerPriority.setDeferred(1, aLayer.get());
    TestOptions::testIntLayerPriority.setDeferred(26, zLayer.get());
    apply();
    RL_CHECK(TestOptions::testIntLayerPriority() == 1);
    aLayer.release();
    zLayer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_layerEnableDisable() {
    OptionLayerHandle layer = acquire({20000, "EnableDisableLayer"});
    RL_CHECK(layer->isEnabled());
    TestOptions::testIntEnableDisable.setDeferred(777, layer.get());
    apply();
    RL_CHECK(TestOptions::testIntEnableDisable() == 777);
    layer->requestEnabled(false);
    RL_CHECK(layer->getPendingEnabled() == false);
    apply();
    RL_CHECK(!layer->isEnabled());
    RL_CHECK(TestOptions::testIntEnableDisable() == 100);
    // Any enable request in a frame wins over disable requests.
    layer->requestEnabled(false);
    layer->requestEnabled(true);
    layer->requestEnabled(false);
    apply();
    RL_CHECK(layer->isEnabled());
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_layerKeyComparison() {
    const OptionLayerKey keyA{100, "LayerA"};
    const OptionLayerKey keyB{200, "LayerB"};
    const OptionLayerKey keyC{100, "LayerC"};
    const OptionLayerKey keyD{100, "LayerA"};
    RL_CHECK(keyB < keyA);
    RL_CHECK(keyA < keyC);
    RL_CHECK(keyA == keyD);
    RL_CHECK(!(keyA == keyB));
    RL_CHECK(!(keyA == keyC));
    RL_CHECK_STR(keyA.toString(), "'LayerA' (priority: 100)");
    // System keys order strongest first.
    RL_CHECK(OptionLayerKey(kQualityLayerId) < OptionLayerKey(kUserLayerId));
    RL_CHECK(OptionLayerKey(kUserLayerId) < OptionLayerKey{kMaxDynamicLayerPriority, "dyn"});
    RL_CHECK(OptionLayerKey{kMinDynamicLayerPriority, "dyn"} < OptionLayerKey(kDerivedLayerId));
    RL_CHECK(OptionLayerKey(kDerivedLayerId) < OptionLayerKey(kEnvironmentLayerId));
    RL_CHECK(OptionLayerKey(kEnvironmentLayerId) < OptionLayerKey(kBaseGameModLayerId));
    RL_CHECK(OptionLayerKey(kBaseGameModLayerId) < OptionLayerKey(kRtxConfLayerId));
    RL_CHECK(OptionLayerKey(kRtxConfLayerId) < OptionLayerKey(kAppConfigLayerId));
    RL_CHECK(OptionLayerKey(kAppConfigLayerId) < OptionLayerKey(kDxvkConfLayerId));
    RL_CHECK(OptionLayerKey(kDxvkConfLayerId) < OptionLayerKey(kDefaultLayerId));
}

void test_hasValueInLayer() {
    OptionLayerHandle layer = acquire({11000, "HasValueLayer"});
    RL_CHECK(TestOptions::testIntObject().hasValueInLayer(OptionLayer::getDefaultLayer()));
    RL_CHECK(!TestOptions::testIntObject().hasValueInLayer(layer.get()));
    RL_CHECK(!layer->hasValues());
    TestOptions::testInt.setDeferred(123, layer.get());
    apply();
    RL_CHECK(TestOptions::testIntObject().hasValueInLayer(layer.get()));
    RL_CHECK(layer->hasValues());
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_multipleLayersComplex() {
    OptionLayerHandle layer1 = acquire({22000, "ComplexLayer1"});
    OptionLayerHandle layer2 = acquire({23000, "ComplexLayer2"});
    OptionLayerHandle layer3 = acquire({24000, "ComplexLayer3"});
    TestOptions::testIntComplex.setDeferred(1000, layer1.get());
    TestOptions::testIntComplex.setDeferred(2000, layer2.get());
    apply();
    RL_CHECK(TestOptions::testIntComplex() == 2000);
    TestOptions::testIntComplex.setDeferred(3000, layer3.get());
    apply();
    RL_CHECK(TestOptions::testIntComplex() == 3000);
    TestOptions::testIntComplexObject().disableLayerValue(layer3.get());
    apply();
    RL_CHECK(TestOptions::testIntComplex() == 2000);
    TestOptions::testIntComplexObject().disableLayerValue(layer2.get());
    apply();
    RL_CHECK(TestOptions::testIntComplex() == 1000);
    TestOptions::testIntComplexObject().disableLayerValue(layer1.get());
    apply();
    RL_CHECK(TestOptions::testIntComplex() == 100);
    layer1.release();
    layer2.release();
    layer3.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_migrateMiscategorizedOptions() {
    OptionLayer* userLayer = const_cast<OptionLayer*>(OptionLayer::getUserLayer());
    OptionLayer* rtxConfLayer = OptionLayer::getRtxConfLayer();
    RL_CHECK(userLayer != nullptr && rtxConfLayer != nullptr);
    if (!userLayer || !rtxConfLayer) {
        return;
    }

    TestOptions::testMigrateDeveloper.setDeferred(999, userLayer);
    apply();
    RL_CHECK(TestOptions::testMigrateDeveloperObject().hasValueInLayer(userLayer));
    RL_CHECK(userLayer->countMiscategorizedOptions() >= 1);

    TestOptions::testMigrateUser.setDeferred(888, rtxConfLayer);
    apply();
    RL_CHECK(TestOptions::testMigrateUserObject().hasValueInLayer(rtxConfLayer));
    RL_CHECK(rtxConfLayer->countMiscategorizedOptions() >= 1);

    RL_CHECK(userLayer->migrateMiscategorizedOptions() >= 1);
    RL_CHECK(!TestOptions::testMigrateDeveloperObject().hasValueInLayer(userLayer));
    RL_CHECK(TestOptions::testMigrateDeveloperObject().hasValueInLayer(rtxConfLayer));
    apply();
    RL_CHECK(TestOptions::testMigrateDeveloper() == 999);

    RL_CHECK(rtxConfLayer->migrateMiscategorizedOptions() >= 1);
    RL_CHECK(!TestOptions::testMigrateUserObject().hasValueInLayer(rtxConfLayer));
    RL_CHECK(TestOptions::testMigrateUserObject().hasValueInLayer(userLayer));
    apply();
    RL_CHECK(TestOptions::testMigrateUser() == 888);

    TestOptions::testMigrateUserNoReset.setDeferred(777, rtxConfLayer);
    apply();
    RL_CHECK(rtxConfLayer->migrateMiscategorizedOptions() >= 1);
    RL_CHECK(!TestOptions::testMigrateUserNoResetObject().hasValueInLayer(rtxConfLayer));
    RL_CHECK(TestOptions::testMigrateUserNoResetObject().hasValueInLayer(userLayer));
    apply();
    RL_CHECK(TestOptions::testMigrateUserNoReset() == 777);

    const Hash64 devHash1 = 0xABCDEF1234567890ull, devHash2 = 0x0987654321FEDCBAull;
    const Hash64 userHash1 = 0x1111222233334444ull, userHash2 = 0x5555666677778888ull;
    TestOptions::testMigrateDeveloperHash.addHash(devHash1, userLayer);
    TestOptions::testMigrateDeveloperHash.addHash(devHash2, userLayer);
    apply();
    RL_CHECK(TestOptions::testMigrateDeveloperHashObject().hasValueInLayer(userLayer));
    RL_CHECK(TestOptions::testMigrateDeveloperHash().count(devHash1) > 0);
    userLayer->migrateMiscategorizedOptions();
    apply();
    RL_CHECK(!TestOptions::testMigrateDeveloperHashObject().hasValueInLayer(userLayer));
    RL_CHECK(TestOptions::testMigrateDeveloperHashObject().hasValueInLayer(rtxConfLayer));
    RL_CHECK(TestOptions::testMigrateDeveloperHash().count(devHash1) > 0);
    RL_CHECK(TestOptions::testMigrateDeveloperHash().count(devHash2) > 0);

    TestOptions::testMigrateUserHash.addHash(userHash1, rtxConfLayer);
    TestOptions::testMigrateUserHash.addHash(userHash2, rtxConfLayer);
    apply();
    rtxConfLayer->migrateMiscategorizedOptions();
    apply();
    RL_CHECK(!TestOptions::testMigrateUserHashObject().hasValueInLayer(rtxConfLayer));
    RL_CHECK(TestOptions::testMigrateUserHashObject().hasValueInLayer(userLayer));
    RL_CHECK(TestOptions::testMigrateUserHash().count(userHash1) > 0);
    RL_CHECK(TestOptions::testMigrateUserHash().count(userHash2) > 0);
    RL_CHECK(userLayer->countMiscategorizedOptions() == 0);
    RL_CHECK(rtxConfLayer->countMiscategorizedOptions() == 0);

    TestOptions::testMigrateDeveloperObject().disableLayerValue(rtxConfLayer);
    TestOptions::testMigrateUserObject().disableLayerValue(userLayer);
    TestOptions::testMigrateUserNoResetObject().disableLayerValue(userLayer);
    TestOptions::testMigrateDeveloperHashObject().disableLayerValue(rtxConfLayer);
    TestOptions::testMigrateUserHashObject().disableLayerValue(userLayer);
    apply();

    TestOptions::testMigrateDeveloper.setDeferred(555, userLayer);
    apply();
    RL_CHECK(userLayer->hasUnsavedChanges());
    userLayer->migrateMiscategorizedOptions();
    RL_CHECK(rtxConfLayer->hasUnsavedChanges());
    TestOptions::testMigrateDeveloperObject().disableLayerValue(rtxConfLayer);
    apply();
    RL_CHECK(!userLayer->hasUnsavedChanges());
    RL_CHECK(!rtxConfLayer->hasUnsavedChanges());
}

void test_floatBlending() {
    OptionLayerHandle layer = acquire({18000, "FloatBlendLayer50"}, 0.5f);
    TestOptions::testFloatBlend.setDeferred(10.0f, layer.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testFloatBlend(), 10.0f * 0.5f + 1.5f * 0.5f, 0.01f);
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_floatBlendChain() {
    // v = lerp(lerp(C, B, 0.5), A, 0.2) with A strongest; D (full strength, below C) is never reached.
    OptionLayerHandle a = acquire({18400, "BlendA"}, 0.2f);
    OptionLayerHandle b = acquire({18300, "BlendB"}, 0.5f);
    OptionLayerHandle c = acquire({18200, "BlendC"}, 1.0f);
    OptionLayerHandle d = acquire({18100, "BlendD"}, 1.0f);
    TestOptions::testFloatBlend.setDeferred(10.0f, a.get());
    TestOptions::testFloatBlend.setDeferred(20.0f, b.get());
    TestOptions::testFloatBlend.setDeferred(40.0f, c.get());
    TestOptions::testFloatBlend.setDeferred(1000.0f, d.get());
    apply();
    RL_CHECK_NEAR(TestOptions::testFloatBlend(), 30.0f * 0.8f + 10.0f * 0.2f, 0.001f);
    // A layer at strength 0 contributes nothing.
    a.get()->requestBlendStrength(0.0f);
    apply();
    RL_CHECK_NEAR(TestOptions::testFloatBlend(), 30.0f, 0.001f);
    for (OptionLayerHandle* layer : {&a, &b, &c, &d}) {
        layer->release();
    }
    apply();
    verifyOptionsAtDefaults();
}

void test_vectorBlending() {
    OptionLayerHandle layer = acquire({19000, "Vector3BlendLayer"}, 0.5f);
    TestOptions::testVector3Blend.setDeferred(Vec3f(10.0f, 20.0f, 30.0f), layer.get());
    apply();
    const Vec3f result = TestOptions::testVector3Blend();
    RL_CHECK_NEAR(result.x, 5.5f, 0.01f);
    RL_CHECK_NEAR(result.y, 11.0f, 0.01f);
    RL_CHECK_NEAR(result.z, 16.5f, 0.01f);
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_blendThreshold() {
    OptionLayerHandle inactive = acquire({21000, "InactiveLayer"}, 0.4f, 0.5f);
    RL_CHECK(TestOptions::testIntThreshold.getDefaultValue() == 100);
    TestOptions::testIntThreshold.setDeferred(9999, inactive.get());
    apply();
    RL_CHECK(!inactive->isActive());
    RL_CHECK(TestOptions::testIntThreshold() == 100);
    // Inactive layers are skipped by forEachLayerValue unless asked for.
    int visited = 0;
    TestOptions::testIntThresholdObject().forEachLayerValue([&](const OptionLayer*, const OptionValue&) {
        ++visited;
        return true;
    });
    RL_CHECK(visited == 1); // default layer only
    visited = 0;
    TestOptions::testIntThresholdObject().forEachLayerValue(
        [&](const OptionLayer*, const OptionValue&) {
            ++visited;
            return true;
        },
        std::nullopt, true);
    RL_CHECK(visited == 2);

    // Hash sets honour the threshold too.
    TestOptions::testHashSet.addHash(0x42, inactive.get());
    apply();
    RL_CHECK(!TestOptions::testHashSet.containsHash(0x42));
    inactive.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_blendStrengthRequest() {
    OptionLayerHandle layer = acquire({9500, "BlendStrengthLayer"}, 0.5f);
    RL_CHECK_NEAR(layer->getBlendStrength(), 0.5f, 0.0001f);
    layer->requestBlendStrength(0.8f);
    RL_CHECK_NEAR(layer->getPendingBlendStrength(), 0.8f, 0.0001f);
    apply();
    RL_CHECK_NEAR(layer->getBlendStrength(), 0.8f, 0.0001f);
    layer->requestBlendStrength(0.3f);
    layer->requestBlendStrength(0.9f);
    layer->requestBlendStrength(0.6f);
    apply();
    RL_CHECK_NEAR(layer->getBlendStrength(), 0.9f, 0.0001f);
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_blendThresholdRequest() {
    OptionLayerHandle layer = acquire({9600, "BlendThresholdLayer"}, 1.0f, 0.5f);
    RL_CHECK_NEAR(layer->getBlendStrengthThreshold(), 0.5f, 0.0001f);
    layer->requestBlendThreshold(0.3f);
    apply();
    RL_CHECK_NEAR(layer->getBlendStrengthThreshold(), 0.3f, 0.0001f);
    layer->requestBlendThreshold(0.8f);
    layer->requestBlendThreshold(0.2f);
    layer->requestBlendThreshold(0.6f);
    apply();
    RL_CHECK_NEAR(layer->getBlendStrengthThreshold(), 0.2f, 0.0001f);
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_optionFlags() {
    RL_CHECK((TestOptions::testIntNoSaveObject().getFlags() & OptionFlags::NoSave) != 0);
    OptionConfig written;
    OptionManager::writeOptions(written, OptionLayer::getDefaultLayer(), false);
    RL_CHECK(!written.contains("rtx.test.testIntNoSave"));
    RL_CHECK(written.contains("rtx.test.testInt"));

    RL_CHECK((TestOptions::testIntNoResetObject().getFlags() & OptionFlags::NoReset) != 0);
    OptionLayerHandle layer = acquire({9500, "NoResetTestLayer"});
    TestOptions::testInt.setDeferred(888, layer.get());
    TestOptions::testIntNoReset.setDeferred(999, layer.get());
    apply();
    RL_CHECK(TestOptions::testInt() == 888);
    RL_CHECK(TestOptions::testIntNoReset() == 999);
    layer->requestEnabled(false);
    apply();
    RL_CHECK(TestOptions::testInt() == 100);
    RL_CHECK(TestOptions::testIntNoReset() == 999);
    layer->requestEnabled(true);
    apply();
    RL_CHECK(TestOptions::testIntNoReset() == 999);
    layer.release();
    apply();
    RL_CHECK(TestOptions::testInt() == 100);
    RL_CHECK(TestOptions::testIntNoReset() == 42);

    // NoSave routes every write to the Derived layer.get(), even an explicit one.
    OptionLayerHandle other = acquire({9700, "NoSaveExplicitLayer"});
    TestOptions::testIntNoSave.setDeferred(5, other.get());
    apply();
    RL_CHECK(!TestOptions::testIntNoSaveObject().hasValueInLayer(other.get()));
    RL_CHECK(TestOptions::testIntNoSaveObject().hasValueInLayer(OptionLayer::getDerivedLayer()));
    RL_CHECK(TestOptions::testIntNoSave() == 5);
    TestOptions::testIntNoSaveObject().disableLayerValue(OptionLayer::getDerivedLayer());
    other.release();
    apply();
    RL_CHECK(TestOptions::testIntNoSave() == 42);
    verifyOptionsAtDefaults();
}

void test_invalidationScope() {
    constexpr std::uint32_t kTestFlagA = 0x1000;
    constexpr std::uint32_t kTestFlagB = 0x2000;
    RL_CHECK(TestOptions::testScopeReadObject().getFlags() == 0);
    (void)TestOptions::testScopeRead();
    RL_CHECK(TestOptions::testScopeReadObject().getFlags() == 0);
    {
        FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(kTestFlagA);
        (void)TestOptions::testScopeRead();
    }
    RL_CHECK((TestOptions::testScopeReadObject().getFlags() & kTestFlagA) != 0);
    {
        FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(kTestFlagA);
    }
    RL_CHECK(TestOptions::testScopeUnreadObject().getFlags() == 0);
    {
        FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(kTestFlagA);
        {
            FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(kTestFlagB);
            (void)TestOptions::testScopeUnread();
        }
    }
    RL_CHECK((TestOptions::testScopeUnreadObject().getFlags() & kTestFlagA) != 0);
    RL_CHECK((TestOptions::testScopeUnreadObject().getFlags() & kTestFlagB) != 0);
    {
        FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(kTestFlagA);
        RL_CHECK(OptionInvalidationScope::getRequiredFlags() == kTestFlagA);
        {
            FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(kTestFlagB);
            RL_CHECK(OptionInvalidationScope::getRequiredFlags() == (kTestFlagA | kTestFlagB));
        }
        RL_CHECK(OptionInvalidationScope::getRequiredFlags() == kTestFlagA);
    }
    RL_CHECK(OptionInvalidationScope::getRequiredFlags() == 0);
    {
        FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(kTestFlagB);
        std::lock_guard<std::recursive_mutex> lock(fuse::relight::options::detail::optionMutex());
        (void)TestOptions::testScopeRead.getNoLock();
    }
    RL_CHECK((TestOptions::testScopeReadObject().getFlags() & kTestFlagB) != 0);
    RL_CHECK(TestOptions::testScopeHashSetObject().getFlags() == 0);
    {
        FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(kTestFlagA);
        (void)TestOptions::testScopeHashSet.containsHash(12345);
    }
    RL_CHECK((TestOptions::testScopeHashSetObject().getFlags() & kTestFlagA) != 0);
}

void test_isDefault() {
    OptionLayerHandle layer = acquire({8000, "IsDefaultLayer"});
    RL_CHECK(TestOptions::testIntObject().isDefault());
    TestOptions::testInt.setDeferred(999, layer.get());
    apply();
    RL_CHECK(!TestOptions::testIntObject().isDefault());
    TestOptions::testInt.setDeferred(100, layer.get());
    apply();
    RL_CHECK(TestOptions::testIntObject().isDefault());
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_resetToDefault() {
    OptionLayerHandle layer = acquire({12000, "ResetLayer"});
    TestOptions::testInt.setDeferred(999, layer.get());
    apply();
    RL_CHECK(TestOptions::testInt() == 999);
    // resetToDefault() writes the default into the current target layer (Derived here), which is
    // weaker than the test layer: the value stays until that layer goes.
    TestOptions::testInt.resetToDefault();
    apply();
    RL_CHECK(TestOptions::testIntObject().hasValueInLayer(OptionLayer::getDerivedLayer()));
    RL_CHECK(TestOptions::testInt() == 999);
    layer.release();
    apply();
    RL_CHECK(TestOptions::testInt() == 100);
    TestOptions::testIntObject().disableLayerValue(OptionLayer::getDerivedLayer());
    apply();
    verifyOptionsAtDefaults();
}

void test_configSerialization() {
    OptionLayerHandle layer = acquire({25000, "SerializeTestLayer"});
    TestOptions::testInt.setDeferred(9999, layer.get());
    TestOptions::testFloat.setDeferred(1.234f, layer.get());
    TestOptions::testBool.setDeferred(true, layer.get());
    TestOptions::testString.setDeferred(std::string("SerializedString"), layer.get());
    TestOptions::testVector3.setDeferred(Vec3f(7.0f, 8.0f, 9.0f), layer.get());
    apply();
    OptionConfig config;
    OptionManager::writeOptions(config, layer.get(), false);
    RL_CHECK(config.get<std::int32_t>("rtx.test.testInt", 0) == 9999);
    RL_CHECK_NEAR(config.get<float>("rtx.test.testFloat", 0.0f), 1.234f, 0.001f);
    RL_CHECK(config.get<bool>("rtx.test.testBool", false));
    RL_CHECK_STR(config.get<std::string>("rtx.test.testString", ""), "SerializedString");
    RL_CHECK(config.get<Vec3f>("rtx.test.testVector3") == Vec3f(7.0f, 8.0f, 9.0f));
    RL_CHECK(config.size() == 5); // only this layer's values
    // changedOptionsOnly drops values equal to what the weaker layers give.
    TestOptions::testBool.setDeferred(false, layer.get());
    apply();
    OptionConfig changed;
    OptionManager::writeOptions(changed, layer.get(), true);
    RL_CHECK(!changed.contains("rtx.test.testBool"));
    RL_CHECK(changed.contains("rtx.test.testInt"));
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_configFileIO() {
    const std::string path = (s_tempDir / "test_rtx_option_temp.conf").string();
    OptionLayerHandle writeLayer = acquire({26000, "FileWriteTestLayer"});
    TestOptions::testInt.setDeferred(77777, writeLayer.get());
    TestOptions::testFloat.setDeferred(2.71828f, writeLayer.get());
    TestOptions::testBool.setDeferred(true, writeLayer.get());
    TestOptions::testString.setDeferred(std::string("FileTestValue"), writeLayer.get());
    TestOptions::testVector2.setDeferred(Vec2f(11.0f, 22.0f), writeLayer.get());
    TestOptions::testVector3.setDeferred(Vec3f(33.0f, 44.0f, 55.0f), writeLayer.get());
    TestOptions::testVector2i.setDeferred(Vec2i(111, 222), writeLayer.get());
    TestOptions::testHashVector.setDeferred(HashVector{3, 1, 2}, writeLayer.get());
    apply();
    OptionConfig writeConfig;
    OptionManager::writeOptions(writeConfig, writeLayer.get(), false);
    RL_CHECK(writeConfig.saveFile(path, defaultSaveKeyFilters()));
    writeLayer.release();
    apply();
    verifyOptionsAtDefaults();

    OptionConfig readConfig = OptionConfig::loadFile(path, OptionSystem::parseOptions());
    RL_CHECK(readConfig.get<std::int32_t>("rtx.test.testInt", 0) == 77777);
    OptionLayerHandle readLayer = OptionManager::acquireLayer(path, {27000, "FileReadTestLayer"}, 1.0f, 0.1f, false,
                                                         &readConfig);
    RL_CHECK(readLayer->isValid());
    apply();
    RL_CHECK(TestOptions::testInt() == 77777);
    RL_CHECK_NEAR(TestOptions::testFloat(), 2.71828f, 0.00001f);
    RL_CHECK(TestOptions::testBool());
    RL_CHECK_STR(TestOptions::testString(), "FileTestValue");
    RL_CHECK(TestOptions::testVector2() == Vec2f(11.0f, 22.0f));
    RL_CHECK(TestOptions::testVector3() == Vec3f(33.0f, 44.0f, 55.0f));
    RL_CHECK(TestOptions::testVector2i() == Vec2i(111, 222));
    RL_CHECK((TestOptions::testHashVector() == HashVector{3, 1, 2}));
    readLayer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_configScalarToVectorPromotion() {
    const std::string path = (s_tempDir / "test_rtx_option_scalar_promotion.conf").string();
    {
        std::ofstream out(path, std::ios::binary);
        out << "rtx.test.testVector3 = 7.0\n";
    }
    OptionConfig config = OptionConfig::loadFile(path, OptionSystem::parseOptions());
    RL_CHECK(!config.empty());
    OptionLayerHandle layer =
        OptionManager::acquireLayer(path, {27500, "ScalarPromotionTestLayer"}, 1.0f, 0.1f, false, &config);
    RL_CHECK(layer->isValid());
    apply();
    RL_CHECK(TestOptions::testVector3() == Vec3f(7.0f));
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

// ============================================================================
// Relight additions
// ============================================================================

void test_hashVectorDoesNotMerge() {
    OptionLayerHandle low = acquire({5000, "HashVectorLow"});
    OptionLayerHandle high = acquire({5100, "HashVectorHigh"});
    TestOptions::testHashVector.setDeferred(HashVector{1, 2, 3}, low.get());
    TestOptions::testHashVector.setDeferred(HashVector{9}, high.get());
    apply();
    RL_CHECK((TestOptions::testHashVector() == HashVector{9}));
    TestOptions::testHashVectorObject().disableLayerValue(high.get());
    apply();
    RL_CHECK((TestOptions::testHashVector() == HashVector{1, 2, 3}));
    low.release();
    high.release();
    apply();
    RL_CHECK(TestOptions::testHashVector().empty());
}

void test_hashSetAssignWholeSet() {
    OptionLayerHandle layer = acquire({5200, "HashAssignLayer"});
    TestOptions::testHashSet.removeHash(0x5, layer.get());
    TestOptions::testHashSet.setDeferred(HashSet{0x1, 0x2}, layer.get());
    apply();
    RL_CHECK((TestOptions::testHashSet() == HashSet{0x1, 0x2}));
    const std::optional<OptionValue> value = TestOptions::testHashSetObject().getLayerValue(layer.get());
    RL_CHECK(value && std::get<HashSetLayer>(*value).hasNegative(0x5)); // negatives survive
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_editTargetRouting() {
    const OptionLayer* user = OptionLayer::getUserLayer();
    const OptionLayer* rtxConf = OptionLayer::getRtxConfLayer();
    const OptionLayer* quality = OptionLayer::getQualityLayer();
    const OptionLayer* derived = OptionLayer::getDerivedLayer();
    RL_CHECK(user && rtxConf && quality && derived);

    // Code-driven (default target).
    RL_CHECK(OptionLayerTarget::current() == OptionEditTarget::Derived);
    RL_CHECK(TestOptions::testMigrateDeveloperObject().getTargetLayer() == derived);
    RL_CHECK(TestOptions::testMigrateUserObject().getTargetLayer() == quality);
    OptionManager::setGraphicsPresetIsCustom(true);
    RL_CHECK(TestOptions::testMigrateUserObject().getTargetLayer() == user);
    OptionManager::setGraphicsPresetIsCustom(false);

    // User-driven.
    {
        OptionLayerTarget target(OptionEditTarget::User);
        RL_CHECK(TestOptions::testMigrateDeveloperObject().getTargetLayer() == rtxConf);
        RL_CHECK(TestOptions::testMigrateUserObject().getTargetLayer() == user);
        RL_CHECK(TestOptions::testIntNoSaveObject().getTargetLayer() == derived); // NoSave overrides
        TestOptions::testMigrateDeveloper.setDeferred(1, nullptr);
        TestOptions::testMigrateUser.setDeferred(2, nullptr);
        {
            OptionLayerTarget nested(OptionEditTarget::Derived);
            RL_CHECK(TestOptions::testMigrateDeveloperObject().getTargetLayer() == derived);
        }
        RL_CHECK(OptionLayerTarget::current() == OptionEditTarget::User);
    }
    RL_CHECK(OptionLayerTarget::current() == OptionEditTarget::Derived);
    apply();
    RL_CHECK(TestOptions::testMigrateDeveloperObject().hasValueInLayer(rtxConf));
    RL_CHECK(TestOptions::testMigrateUserObject().hasValueInLayer(user));
    RL_CHECK(TestOptions::testMigrateDeveloper() == 1 && TestOptions::testMigrateUser() == 2);

    // The quality layer is the strongest: a code-driven preset value hides the user's choice.
    TestOptions::testMigrateUser.setDeferred(3);
    apply();
    RL_CHECK(TestOptions::testMigrateUser() == 3);
    RL_CHECK(TestOptions::testMigrateUserObject().getBlockingLayer(user) == quality);

    TestOptions::testMigrateDeveloperObject().disableLayerValue(rtxConf);
    TestOptions::testMigrateUserObject().disableLayerValue(user);
    TestOptions::testMigrateUserObject().disableLayerValue(quality);
    apply();
    RL_CHECK(TestOptions::testMigrateDeveloper() == 100 && TestOptions::testMigrateUser() == 200);
}

void test_blockingAndClearStronger() {
    OptionLayerHandle low = acquire({4000, "ClearLow"});
    OptionLayerHandle mid = acquire({4100, "ClearMid"});
    OptionLayerHandle high = acquire({4200, "ClearHigh"});
    TestOptions::testIntComplex.setDeferred(1, low.get());
    TestOptions::testIntComplex.setDeferred(2, mid.get());
    TestOptions::testIntComplex.setDeferred(3, high.get());
    TestOptions::testHashSet.addHash(0x10, mid.get());
    TestOptions::testHashSet.addHash(0x20, high.get());
    TestOptions::testHashSet.removeHash(0x10, high.get());
    apply();
    RL_CHECK(TestOptions::testIntComplexObject().getBlockingLayer(low.get()) == high.get());
    RL_CHECK(TestOptions::testIntComplexObject().getBlockingLayer(high.get()) == nullptr);
    RL_CHECK(TestOptions::testHashSetObject().getBlockingLayer(mid.get(), Hash64{0x10}) == high.get());
    RL_CHECK(TestOptions::testHashSetObject().getBlockingLayer(mid.get(), Hash64{0x30}) == nullptr);

    TestOptions::testIntComplexObject().clearFromStrongerLayers(low.get());
    apply();
    RL_CHECK(TestOptions::testIntComplex() == 1);
    RL_CHECK(!TestOptions::testIntComplexObject().hasValueInLayer(mid.get()));

    // For hash sets only the given hash is cleared from stronger layers.
    TestOptions::testHashSetObject().clearFromStrongerLayers(mid.get(), Hash64{0x10});
    apply();
    RL_CHECK(TestOptions::testHashSet.containsHash(0x10));
    RL_CHECK(TestOptions::testHashSet.containsHash(0x20));

    for (OptionLayerHandle* layer : {&low, &mid, &high}) {
        layer->release();
    }
    apply();
    verifyOptionsAtDefaults();
}

void test_removeRedundantLayerValues() {
    OptionLayerHandle low = acquire({4300, "RedundantLow"});
    OptionLayerHandle high = acquire({4400, "RedundantHigh"});
    TestOptions::testIntComplex.setDeferred(5, low.get());
    TestOptions::testIntComplex.setDeferred(5, high.get());  // same as below: redundant
    TestOptions::testInt.setDeferred(100, high.get());        // same as the default: redundant
    TestOptions::testFloat.setDeferred(2.0f, high.get());     // real change
    apply();
    RL_CHECK(TestOptions::testIntComplexObject().isLayerValueRedundant(high.get()));
    RL_CHECK(!TestOptions::testFloatObject().isLayerValueRedundant(high.get()));
    RL_CHECK(OptionManager::removeRedundantLayerValues(high.get()) == 2);
    apply();
    RL_CHECK(!TestOptions::testIntComplexObject().hasValueInLayer(high.get()));
    RL_CHECK(TestOptions::testFloatObject().hasValueInLayer(high.get()));
    RL_CHECK(TestOptions::testIntComplex() == 5);
    RL_CHECK(high->hasValues());
    low.release();
    high.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_migrateValuesTo() {
    OptionLayerHandle layer = acquire({4500, "MigrateValuesLayer"});
    OptionLayerHandle skipped = acquire({4600, "MigrateSkippedLayer"});
    TestOptions::testMigrateSource.setDeferred(8, layer.get());
    TestOptions::testMigrateSource.setDeferred(-1, skipped.get());
    apply();
    const bool migrated = TestOptions::testMigrateSourceObject().migrateValuesTo(
        &TestOptions::testMigrateDestinationObject(), [](const OptionValue& source, OptionValue& destination, bool) {
            const std::int32_t value = std::get<std::int32_t>(source);
            if (value < 0) {
                return false; // declined
            }
            destination.emplace<float>(static_cast<float>(value) * 0.5f);
            return true;
        });
    RL_CHECK(migrated);
    apply();
    RL_CHECK_NEAR(TestOptions::testMigrateDestination(), 4.0f, 0.0001f);
    RL_CHECK(TestOptions::testMigrateDestinationObject().hasValueInLayer(layer.get()));
    RL_CHECK(!TestOptions::testMigrateDestinationObject().hasValueInLayer(skipped.get())); // no zero left behind
    layer.release();
    skipped.release();
    apply();
    RL_CHECK_NEAR(TestOptions::testMigrateDestination(), 0.0f, 0.0001f);
}

void test_setImmediately() {
    OptionLayerHandle layer = acquire({4700, "ImmediateLayer"});
    s_intCallbackCount = 0;
    TestOptions::testIntWithCallback.setImmediately(42, layer.get());
    RL_CHECK(TestOptions::testIntWithCallback() == 42); // visible before the end of the frame
    RL_CHECK(TestOptions::testIntWithCallbackObject().isDirty());
    apply();
    RL_CHECK(s_intCallbackCount == 0); // already resolved, so no change is seen at end of frame
    layer.release();
    apply();
    RL_CHECK(TestOptions::testIntWithCallback() == 0);
    s_intCallbackCount = 0;
}

void test_drawcallInvalidation() {
    OptionManager::clearDrawcallTranslationInvalid();
    OptionLayerHandle layer = acquire({4800, "DrawcallLayer"});
    TestOptions::testInt.setDeferred(1, layer.get());
    apply();
    RL_CHECK(!OptionManager::isDrawcallTranslationInvalid());
    TestOptions::testIntDrawcall.setDeferred(1, layer.get());
    apply();
    RL_CHECK(OptionManager::isDrawcallTranslationInvalid());
    OptionManager::clearDrawcallTranslationInvalid();
    RL_CHECK(!OptionManager::isDrawcallTranslationInvalid());
    layer.release();
    apply();
    OptionManager::clearDrawcallTranslationInvalid();
    verifyOptionsAtDefaults();
}

void test_aliases() {
    // relight.* <-> rtx.* twins resolve in both directions.
    RL_CHECK(OptionManager::findOption("rtx.test.testInt") == &TestOptions::testIntObject());
    RL_CHECK(OptionManager::findOption("relight.test.testInt") == &TestOptions::testIntObject());
    RL_CHECK(OptionManager::findOption("relight.test.nativeOnly") == &TestOptions::nativeOnlyObject());
    RL_CHECK(OptionManager::findOption("rtx.test.nativeOnly") == &TestOptions::nativeOnlyObject());
    RL_CHECK(OptionManager::findOption("rtx.test.doesNotExist") == nullptr);
    RL_CHECK_STR(OptionManager::twinName("rtx.a.b"), "relight.a.b");
    RL_CHECK_STR(OptionManager::twinName("relight.a"), "rtx.a");
    RL_CHECK_STR(OptionManager::twinName("d3d9.a"), "");

    OptionManager::addAlias("rtx.test.oldIntName", "rtx.test.testIntComplex");
    RL_CHECK(OptionManager::findOption("rtx.test.oldIntName") == &TestOptions::testIntComplexObject());
    const std::vector<std::string> keys = TestOptions::testIntComplexObject().getConfigKeys();
    RL_CHECK(keys.size() == 3 && keys[0] == "rtx.test.testIntComplex" && keys[1] == "relight.test.testIntComplex" &&
             keys[2] == "rtx.test.oldIntName");

    // Config files may use any of the names; the declared name wins when several are present.
    OptionConfig config;
    config.set("relight.test.testInt", "5");
    config.set("rtx.test.oldIntName", "6");
    config.set("rtx.test.nativeOnly", "8");
    config.set("rtx.test.testFloat", "2.5");
    config.set("relight.test.testFloat", "9.5");
    OptionLayerHandle layer = OptionManager::acquireLayer("", {4900, "AliasLayer"}, 1.0f, 0.1f, false, &config);
    apply();
    RL_CHECK(TestOptions::testInt() == 5);
    RL_CHECK(TestOptions::testIntComplex() == 6);
    RL_CHECK(TestOptions::nativeOnly() == 8);
    RL_CHECK_NEAR(TestOptions::testFloat(), 2.5f, 0.0001f);
    // Written back under the declared names.
    OptionConfig written;
    OptionManager::writeOptions(written, layer.get(), false);
    RL_CHECK(written.contains("rtx.test.testInt") && !written.contains("relight.test.testInt"));
    RL_CHECK(written.contains("relight.test.nativeOnly"));
    layer.release();
    OptionManager::removeAlias("rtx.test.oldIntName");
    RL_CHECK(OptionManager::findOption("rtx.test.oldIntName") == nullptr);
    apply();
    verifyOptionsAtDefaults();
}

void test_malformedValuesMatchUpstream() {
    // Upstream inserts the type's parse result even when parsing fails: an int that does not parse
    // becomes 0 in that layer.get(), and a short vector keeps the components it did parse.
    OptionConfig config;
    config.set("rtx.test.testIntComplex", "abc");
    config.set("rtx.test.testVector3", "5, 6");
    config.set("rtx.test.testBool", "yes");
    config.set("rtx.test.testHashSet", "0x1, garbage, -0x2");
    OptionLayerHandle layer = OptionManager::acquireLayer("", {5300, "MalformedLayer"}, 1.0f, 0.1f, false, &config);
    apply();
    RL_CHECK(TestOptions::testIntComplex() == 0);
    RL_CHECK(TestOptions::testVector3() == Vec3f(5.0f, 6.0f, 0.0f));
    RL_CHECK(TestOptions::testBool() == false);
    RL_CHECK(TestOptions::testHashSet.containsHash(0x1)); // valid entries kept, bad ones skipped
    const std::optional<OptionValue> hashes = TestOptions::testHashSetObject().getLayerValue(layer.get());
    RL_CHECK(hashes && std::get<HashSetLayer>(*hashes).hasNegative(0x2));
    layer.release();
    apply();
    verifyOptionsAtDefaults();
}

void test_layerHandle() {
    const OptionLayerKey key{5400, "HandleLayer"};
    {
        OptionLayerHandle handle = acquire(key);
        RL_CHECK(static_cast<bool>(handle) && OptionManager::getLayer(key) == handle.get());
        TestOptions::testIntComplex.setDeferred(5, handle.get());
        apply();
        RL_CHECK(TestOptions::testIntComplex() == 5);
        OptionLayerHandle moved = std::move(handle); // moving transfers the reference
        RL_CHECK(!handle && moved.get() == OptionManager::getLayer(key));
        OptionLayerHandle second = acquire(key);     // a second reference to the same layer
        moved.release();
        RL_CHECK(!moved && OptionManager::getLayer(key) == second.get());
        apply();
        RL_CHECK(TestOptions::testIntComplex() == 5);
        moved.release(); // no-op on an empty handle
        OptionLayerHandle assigned;
        assigned = std::move(second);
        RL_CHECK(assigned.get() != nullptr);
    } // last handle destroyed: layer and its values go
    RL_CHECK(OptionManager::getLayer(key) == nullptr);
    apply();
    RL_CHECK(TestOptions::testIntComplex() == 100);
}

void test_dynamicLayerPriorityClamp() {
    OptionLayerHandle low = OptionManager::acquireLayer("", {5, "ClampedLow"});
    OptionLayerHandle high = OptionManager::acquireLayer("", {0xFFFFFFF0u, "ClampedHigh"});
    RL_CHECK(low->getLayerKey().priority == kMinDynamicLayerPriority);
    RL_CHECK(high->getLayerKey().priority == kMaxDynamicLayerPriority);
    // Acquiring the same key again references the same layer.
    OptionLayerHandle again = OptionManager::acquireLayer("", {5, "ClampedLow"});
    RL_CHECK(again.get() == low.get());
    again.release();
    RL_CHECK(OptionManager::getLayer(low->getLayerKey()) == low.get());
    low.release();
    high.release();
    RL_CHECK(OptionManager::getLayer({kMinDynamicLayerPriority, "ClampedLow"}) == nullptr);
    RL_CHECK(clampComponentLayerPriority(-5.0f) == kMinDynamicLayerPriority);
    RL_CHECK(clampComponentLayerPriority(12345.4f) == 12345u);
    RL_CHECK(clampComponentLayerPriority(1.0e9f) == kMaxDynamicLayerPriority);
}

void test_markdownDocumentation() {
    const std::string doc = OptionManager::generateMarkdownDocumentation();
    RL_CHECK(doc.find("## Simple Types") != std::string::npos);
    RL_CHECK(doc.find("## Complex Types") != std::string::npos);
    RL_CHECK(doc.find("|rtx.test.testIntWithMinMax|int|50|0|100|Test int with min and max|") != std::string::npos);
    RL_CHECK(doc.find("|rtx.test.testVector2|float2|1, 2|||Test Vector2 option|") != std::string::npos);
    RL_CHECK(doc.find("|rtx.test.testHashSet|hash set||||Test hash set option|") != std::string::npos);
    RL_CHECK(doc.find("Test int with env and NoSave flag") != std::string::npos);
    // Markdown-significant characters in descriptions are escaped.
    RL_CHECK(doc.find("Cyclic option A that adjusts B's bounds") != std::string::npos);
    RL_CHECK(doc.find("Value chain D \\(end of chain\\)") != std::string::npos);
    const std::string path = (s_tempDir / "relight-options.md").string();
    RL_CHECK(OptionManager::writeMarkdownDocumentation(path));
    RL_CHECK(std::filesystem::file_size(path) == doc.size());
}

} // namespace

void runSemanticsTests() {
    s_tempDir = fuse::test::makeUniqueTempDir("rl_options_semantics");

    // Like the upstream test harness: system layers (from an empty directory), then the startup
    // callback pass.
    OptionSystemDesc desc;
    desc.baseDirectory = s_tempDir.string();
    desc.exeName = "rl_options_test.exe";
    OptionSystem::initialize(desc);

    static const TestCase kCases[] = {
        {"basicTypes", test_basicTypes},
        {"setAndGet", test_setAndGet},
        {"getDefaultValue", test_getDefaultValue},
        {"fullOptionName", test_fullOptionName},
        {"optionTypeIdentification", test_optionTypeIdentification},
        {"minMaxClamping", test_minMaxClamping},
        {"dynamicMinMax", test_dynamicMinMax},
        {"onChangeCallback", test_onChangeCallback},
        {"minMaxInterdependency", test_minMaxInterdependency},
        {"chainedOnChangeCallbacks", test_chainedOnChangeCallbacks},
        {"cyclicOnChangeCallbacksTerminate", test_cyclicOnChangeCallbacksTerminate},
        {"valueSettingChain", test_valueSettingChain},
        {"cyclicValueSettingTerminates", test_cyclicValueSettingTerminates},
        {"environmentVariables", test_environmentVariables},
        {"hashSetOperations", test_hashSetOperations},
        {"hashSetLayerMerging", test_hashSetLayerMerging},
        {"layerPriorityOverride", test_layerPriorityOverride},
        {"layerEnableDisable", test_layerEnableDisable},
        {"layerKeyComparison", test_layerKeyComparison},
        {"hasValueInLayer", test_hasValueInLayer},
        {"multipleLayersComplex", test_multipleLayersComplex},
        {"migrateMiscategorizedOptions", test_migrateMiscategorizedOptions},
        {"floatBlending", test_floatBlending},
        {"floatBlendChain", test_floatBlendChain},
        {"vectorBlending", test_vectorBlending},
        {"blendThreshold", test_blendThreshold},
        {"blendStrengthRequest", test_blendStrengthRequest},
        {"blendThresholdRequest", test_blendThresholdRequest},
        {"optionFlags", test_optionFlags},
        {"invalidationScope", test_invalidationScope},
        {"isDefault", test_isDefault},
        {"resetToDefault", test_resetToDefault},
        {"configSerialization", test_configSerialization},
        {"configFileIO", test_configFileIO},
        {"configScalarToVectorPromotion", test_configScalarToVectorPromotion},
        {"hashVectorDoesNotMerge", test_hashVectorDoesNotMerge},
        {"hashSetAssignWholeSet", test_hashSetAssignWholeSet},
        {"editTargetRouting", test_editTargetRouting},
        {"blockingAndClearStronger", test_blockingAndClearStronger},
        {"removeRedundantLayerValues", test_removeRedundantLayerValues},
        {"migrateValuesTo", test_migrateValuesTo},
        {"setImmediately", test_setImmediately},
        {"drawcallInvalidation", test_drawcallInvalidation},
        {"aliases", test_aliases},
        {"malformedValuesMatchUpstream", test_malformedValuesMatchUpstream},
        {"layerHandle", test_layerHandle},
        {"dynamicLayerPriorityClamp", test_dynamicLayerPriorityClamp},
        {"markdownDocumentation", test_markdownDocumentation},
    };
    runSuite("semantics", kCases);

    std::error_code ec;
    std::filesystem::remove_all(s_tempDir, ec);
}

} // namespace rl_options_test
