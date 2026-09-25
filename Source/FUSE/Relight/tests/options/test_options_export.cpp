/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// rl_options / export: port of dxvk-remix tests/rtx/unit/test_option_layer_export.cpp@0867d3c (MIT,
// Copyright (c) NVIDIA CORPORATION), rewritten for the FUSE option API.

#include "rl_options_test.hpp"

#include <fuse/core/temp_path.hpp>
#include <fuse/relight/options/options.hpp>

#include <filesystem>

namespace rl_options_test {
namespace {

using namespace fuse::relight::options;

const OptionLayerKey kTestLayerKey{1000, "TestExportLayer"};

struct TestExportOptions {
    FUSE_RELIGHT_OPTION("rtx.test.export", std::int32_t, testIntOption, 42, "Test integer option for export");
    FUSE_RELIGHT_OPTION("rtx.test.export", float, testFloatOption, 3.14f, "Test float option for export");
    FUSE_RELIGHT_OPTION("rtx.test.export", std::string, testStringOption, "default", "Test string option for export");
    FUSE_RELIGHT_OPTION("rtx.test.export", HashSet, testHashSetOption, {}, "Test hash set option for export");
};

std::filesystem::path s_dir;

std::string path(const char* name) { return (s_dir / name).string(); }

void cleanup(const std::string& file) {
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

std::string readOptionFromFile(const std::string& file, const std::string& option) {
    const OptionConfig config = OptionConfig::loadFile(file, OptionSystem::parseOptions());
    const std::string* value = config.find(option);
    return value ? *value : std::string();
}

HashSetLayer readHashesFromFile(const std::string& file) {
    const OptionConfig config = OptionConfig::loadFile(file, OptionSystem::parseOptions());
    HashSetLayer hashes;
    if (const std::string* value = config.find("rtx.test.export.testHashSetOption")) {
        hashes.parseFromStrings(splitConfigList(*value));
    }
    return hashes;
}

void testExportAddedOptionsNewFile() {
    const std::string exportFile = path("test_export_added_new.conf");
    const std::string layerFile = path("test_layer_source.conf");
    const OptionConfig empty;
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &empty);
    RL_CHECK(layer.get() != nullptr);
    TestExportOptions::testIntOption.setImmediately(100, layer.get());
    TestExportOptions::testFloatOption.setImmediately(2.71f, layer.get());
    TestExportOptions::testStringOption.setImmediately(std::string("test_value"), layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    RL_CHECK(std::filesystem::exists(exportFile));
    RL_CHECK_STR(readOptionFromFile(exportFile, "rtx.test.export.testIntOption"), "100");
    float exported = 0.0f;
    RL_CHECK(parseOptionValue(readOptionFromFile(exportFile, "rtx.test.export.testFloatOption"), exported) &&
             exported == 2.71f);
    RL_CHECK_STR(readOptionFromFile(exportFile, "rtx.test.export.testStringOption"), "test_value");
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testExportModifiedOptions() {
    const std::string exportFile = path("test_export_modified.conf");
    const std::string layerFile = path("test_layer_modified_source.conf");
    OptionConfig initial;
    initial.setValue("rtx.test.export.testIntOption", std::int32_t{42});
    initial.setValue("rtx.test.export.testFloatOption", 3.14f);
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    TestExportOptions::testIntOption.setImmediately(200, layer.get());
    TestExportOptions::testFloatOption.setImmediately(6.28f, layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    RL_CHECK_STR(readOptionFromFile(exportFile, "rtx.test.export.testIntOption"), "200");
    float exported = 0.0f;
    RL_CHECK(parseOptionValue(readOptionFromFile(exportFile, "rtx.test.export.testFloatOption"), exported) &&
             exported == 6.28f);
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testExportHashSetAddNew() {
    const std::string exportFile = path("test_export_hashset_add.conf");
    const std::string layerFile = path("test_layer_hashset_add_source.conf");
    OptionConfig initial;
    initial.set("rtx.test.export.testHashSetOption", "0x1111111111111111, 0x2222222222222222");
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    TestExportOptions::testHashSetOption.setImmediately(
        HashSet{0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull, 0x4444444444444444ull}, layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    const HashSetLayer exported = readHashesFromFile(exportFile);
    RL_CHECK(exported.hasPositive(0x3333333333333333ull));
    RL_CHECK(exported.hasPositive(0x4444444444444444ull));
    RL_CHECK(!exported.hasPositive(0x1111111111111111ull)); // delta export
    RL_CHECK(!exported.hasPositive(0x2222222222222222ull));
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testExportHashSetAddThenRemove() {
    const std::string exportFile = path("test_export_hashset_add_remove.conf");
    const std::string layerFile = path("test_layer_hashset_add_remove_source.conf");
    const OptionConfig initial;
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    TestExportOptions::testHashSetOption.addHash(0x1111111111111111ull, layer.get());
    TestExportOptions::testHashSetOption.removeHash(0x1111111111111111ull, layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    const HashSetLayer exported = readHashesFromFile(exportFile);
    RL_CHECK(exported.hasNegative(0x1111111111111111ull));
    RL_CHECK(!exported.hasPositive(0x1111111111111111ull));
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testExportHashSetConflictingOpinions() {
    const std::string exportFile = path("test_export_hashset_negative.conf");
    const std::string layerFile = path("test_layer_hashset_negative_source.conf");
    OptionConfig initial;
    initial.set("rtx.test.export.testHashSetOption", "0x1111111111111111, 0x2222222222222222");
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    HashSetLayer saved;
    saved.parseFromStrings(splitConfigList(*layer->getConfig().find("rtx.test.export.testHashSetOption")));
    RL_CHECK(saved.hasPositive(0x1111111111111111ull) && saved.hasPositive(0x2222222222222222ull));
    TestExportOptions::testHashSetOption.removeHash(0x1111111111111111ull, layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    const HashSetLayer exported = readHashesFromFile(exportFile);
    RL_CHECK(exported.hasNegative(0x1111111111111111ull));
    RL_CHECK(!exported.hasPositive(0x1111111111111111ull));
    RL_CHECK(!exported.hasPositive(0x2222222222222222ull));
    RL_CHECK(!exported.hasNegative(0x2222222222222222ull));
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testExportHashSetMergeWithExistingFile() {
    const std::string exportFile = path("test_export_hashset_merge.conf");
    const std::string layerFile = path("test_layer_hashset_merge_source.conf");
    OptionConfig existing;
    existing.set("rtx.test.export.testHashSetOption", "0x5555555555555555, 0x6666666666666666");
    RL_CHECK(existing.saveFile(exportFile, defaultSaveKeyFilters()));
    OptionConfig initial;
    initial.set("rtx.test.export.testHashSetOption", "0x1111111111111111");
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    TestExportOptions::testHashSetOption.setImmediately(HashSet{0x1111111111111111ull, 0x7777777777777777ull}, layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    const HashSetLayer exported = readHashesFromFile(exportFile);
    RL_CHECK(exported.hasPositive(0x5555555555555555ull));
    RL_CHECK(exported.hasPositive(0x6666666666666666ull));
    RL_CHECK(exported.hasPositive(0x7777777777777777ull));
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testExportHashSetConflictInMerge() {
    const std::string exportFile = path("test_export_hashset_conflict_merge.conf");
    const std::string layerFile = path("test_layer_hashset_conflict_merge_source.conf");
    OptionConfig existing;
    existing.set("rtx.test.export.testHashSetOption", "0x1111111111111111, 0x2222222222222222");
    RL_CHECK(existing.saveFile(exportFile, defaultSaveKeyFilters()));
    const OptionConfig initial;
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    TestExportOptions::testHashSetOption.addHash(0x2222222222222222ull, layer.get());
    TestExportOptions::testHashSetOption.removeHash(0x1111111111111111ull, layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    HashSetLayer exported = readHashesFromFile(exportFile);
    RL_CHECK(exported.hasNegative(0x1111111111111111ull));
    RL_CHECK(!exported.hasPositive(0x1111111111111111ull));
    RL_CHECK(exported.hasPositive(0x2222222222222222ull));
    RL_CHECK(!exported.hasNegative(0x2222222222222222ull));

    existing.set("rtx.test.export.testHashSetOption", "-0x3333333333333333, 0x4444444444444444");
    RL_CHECK(existing.saveFile(exportFile, defaultSaveKeyFilters()));
    layer.release();
    layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    TestExportOptions::testHashSetOption.addHash(0x3333333333333333ull, layer.get());
    TestExportOptions::testHashSetOption.addHash(0x4444444444444444ull, layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    exported = readHashesFromFile(exportFile);
    RL_CHECK(exported.hasPositive(0x3333333333333333ull));
    RL_CHECK(!exported.hasNegative(0x3333333333333333ull));
    RL_CHECK(exported.hasPositive(0x4444444444444444ull));
    RL_CHECK(!exported.hasNegative(0x4444444444444444ull));
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testExportNonHashSetMergeOverwrite() {
    const std::string exportFile = path("test_export_merge_overwrite.conf");
    const std::string layerFile = path("test_layer_merge_overwrite_source.conf");
    OptionConfig existing;
    existing.setValue("rtx.test.export.testIntOption", std::int32_t{999});
    existing.set("rtx.test.export.testStringOption", "old_value");
    RL_CHECK(existing.saveFile(exportFile, defaultSaveKeyFilters()));
    OptionConfig initial;
    initial.setValue("rtx.test.export.testIntOption", std::int32_t{42});
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    TestExportOptions::testIntOption.setImmediately(300, layer.get());
    RL_CHECK(layer->exportUnsavedChanges(exportFile));
    RL_CHECK_STR(readOptionFromFile(exportFile, "rtx.test.export.testIntOption"), "300");
    RL_CHECK_STR(readOptionFromFile(exportFile, "rtx.test.export.testStringOption"), "old_value");
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testExportNoUnsavedChanges() {
    const std::string exportFile = path("test_export_no_changes.conf");
    const std::string layerFile = path("test_layer_no_changes_source.conf");
    OptionConfig initial;
    initial.setValue("rtx.test.export.testIntOption", std::int32_t{42});
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    RL_CHECK(!layer->hasUnsavedChanges());
    RL_CHECK(!layer->exportUnsavedChanges(exportFile));
    RL_CHECK(!std::filesystem::exists(exportFile));
    layer.release();
    cleanup(exportFile);
    cleanup(layerFile);
}

void testForEachChangeClassification() {
    // forEachChange drives both export and the UI's "View Changes".
    const std::string layerFile = path("test_layer_changes.conf");
    OptionConfig initial;
    initial.setValue("rtx.test.export.testIntOption", std::int32_t{42});         // will be modified
    initial.setValue("rtx.test.export.testFloatOption", 3.14f);                  // will be removed
    initial.set("rtx.test.export.testHashSetOption", "0x2, 0x1");                // unchanged (order-free)
    OptionLayerHandle layer = OptionManager::acquireLayer(layerFile, kTestLayerKey, 1.0f, 0.1f, false, &initial);
    TestExportOptions::testIntOption.setImmediately(43, layer.get());
    TestExportOptions::testFloatOptionObject().disableLayerValue(layer.get());
    TestExportOptions::testStringOption.setImmediately(std::string("added"), layer.get());
    std::vector<std::string> added, modified, removed, unchanged;
    layer->forEachChange([&](OptionBase* o, const OptionValue*) { added.push_back(o->getFullName()); },
                         [&](OptionBase* o, const OptionValue*) { modified.push_back(o->getFullName()); },
                         [&](OptionBase* o, const std::string&) { removed.push_back(o->getFullName()); },
                         [&](OptionBase* o, const OptionValue*) { unchanged.push_back(o->getFullName()); });
    RL_CHECK(added == std::vector<std::string>{"rtx.test.export.testStringOption"});
    RL_CHECK(modified == std::vector<std::string>{"rtx.test.export.testIntOption"});
    RL_CHECK(removed == std::vector<std::string>{"rtx.test.export.testFloatOption"});
    RL_CHECK(unchanged == std::vector<std::string>{"rtx.test.export.testHashSetOption"});
    RL_CHECK(layer->hasUnsavedChanges());
    RL_CHECK(layer->hasPendingRemovals());
    layer.release();
    OptionManager::applyPendingValues(nullptr, false);
    cleanup(layerFile);
}

} // namespace

void runExportTests() {
    s_dir = fuse::test::makeUniqueTempDir("rl_options_export");
    static const TestCase kCases[] = {
        {"exportAddedOptionsNewFile", testExportAddedOptionsNewFile},
        {"exportModifiedOptions", testExportModifiedOptions},
        {"exportHashSetAddNew", testExportHashSetAddNew},
        {"exportHashSetAddThenRemove", testExportHashSetAddThenRemove},
        {"exportHashSetConflictingOpinions", testExportHashSetConflictingOpinions},
        {"exportHashSetMergeWithExistingFile", testExportHashSetMergeWithExistingFile},
        {"exportHashSetConflictInMerge", testExportHashSetConflictInMerge},
        {"exportNonHashSetMergeOverwrite", testExportNonHashSetMergeOverwrite},
        {"exportNoUnsavedChanges", testExportNoUnsavedChanges},
        {"forEachChangeClassification", testForEachChangeClassification},
    };
    runSuite("export", kCases);
    OptionManager::applyPendingValues(nullptr, false);
    RL_CHECK(TestExportOptions::testIntOption() == 42);
    std::error_code ec;
    std::filesystem::remove_all(s_dir, ec);
}

} // namespace rl_options_test
