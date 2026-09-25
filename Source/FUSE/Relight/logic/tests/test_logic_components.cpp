// FUSE Relight RL-3.5: per-component semantics (rl_logic_unit). The Transform cases are rewritten from dxvk-remix
// tests/rtx/unit/test_transform_components.cpp@0867d3c (same inputs and expected values, MIT: facts only); Sense,
// Act and Constants cases exercise the same code paths through FrameInputs / PrimSnapshot and the RL-0.6 options.
#include "test_logic_common.hpp"

#include <fuse/relight/logic/animation_utils.hpp>
#include <fuse/relight/logic/keybind.hpp>
#include <fuse/relight/logic/logic_log.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <cfloat>
#include <filesystem>
#include <fstream>

namespace rl_logic_test {

namespace {

namespace fs = std::filesystem;

struct TestOptions {
    FUSE_RELIGHT_OPTION("rtx.logicTest", bool, flag, true, "RL-3.5 test option (bool).");
    FUSE_RELIGHT_OPTION("rtx.logicTest", float, number, 1.5f, "RL-3.5 test option (float).");
    FUSE_RELIGHT_OPTION("rtx.logicTest", std::int32_t, count, 7, "RL-3.5 test option (int).");
    FUSE_RELIGHT_OPTION("rtx.logicTest", fuse::relight::options::Vec2f, v2, fuse::relight::options::Vec2f(1.0f, 2.0f),
                        "RL-3.5 test option (Vector2).");
    FUSE_RELIGHT_OPTION("rtx.logicTest", fuse::relight::options::Vec3f, v3, fuse::relight::options::Vec3f(1.0f, 2.0f, 3.0f),
                        "RL-3.5 test option (Vector3).");
    FUSE_RELIGHT_OPTION("rtx.logicTest", fuse::relight::options::Vec4f, v4,
                        fuse::relight::options::Vec4f(1.0f, 2.0f, 3.0f, 4.0f), "RL-3.5 test option (Vector4).");
};

using Types = std::map<std::string, PropertyType>;
template <typename T>
PropertyVector vec(std::initializer_list<T> v) {
    return std::vector<T>(v);
}
using U = std::uint32_t;

// ---- Arithmetic --------------------------------------------------------------------------------------------------

void testArithmetic() {
    CHECK(variantCount("Add") == 4);
    CHECK(variantCount("Subtract") == 4);
    CHECK(variantCount("Multiply") == 10);
    CHECK(variantCount("Divide") == 7);
    {
        Direct d(variantOf("Add", {{"a", PT::Float}, {"b", PT::Float}, {"sum", PT::Float}}),
                 {vec<float>({2.5f, 10.0f, -5.0f}), vec<float>({1.5f, -3.0f, 5.0f}), vec<float>({0, 0, 0})});
        d.update(0, 3);
        CHECK(feq(d.at<float>(2)[0], 4.0f) && feq(d.at<float>(2)[1], 7.0f) && feq(d.at<float>(2)[2], 0.0f));
    }
    {
        Direct d(variantOf("Add", {{"a", PT::Float2}, {"b", PT::Float2}}),
                 {vec<Vector2>({{1, 2}, {-1, -2}}), vec<Vector2>({{3, 4}, {1, 2}}), vec<Vector2>({{}, {}})});
        d.update(0, 2);
        CHECK(veq(d.at<Vector2>(2)[0], {4, 6}) && veq(d.at<Vector2>(2)[1], {0, 0}));
    }
    {
        Direct d(variantOf("Add", {{"a", PT::Float3}, {"b", PT::Float3}}),
                 {vec<Vector3>({{1, 2, 3}, {-1, -2, -3}}), vec<Vector3>({{4, 5, 6}, {1, 2, 3}}), vec<Vector3>({{}, {}})});
        d.update(0, 2);
        CHECK(veq(d.at<Vector3>(2)[0], {5, 7, 9}) && veq(d.at<Vector3>(2)[1], {0, 0, 0}));
    }
    {
        Direct d(variantOf("Add", {{"a", PT::Float4}, {"b", PT::Float4}}),
                 {vec<Vector4>({{1, 2, 3, 4}}), vec<Vector4>({{5, 6, 7, 8}}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(2)[0], {6, 8, 10, 12}));
    }
    CHECK(variantOf("Add", {{"a", PT::Float3}, {"b", PT::Float}}) == nullptr); // vector + number does not compile upstream
    {
        Direct d(variantOf("Subtract", {{"a", PT::Float}, {"b", PT::Float}}),
                 {vec<float>({10, 5, -3}), vec<float>({3, 2, -5}), vec<float>({0, 0, 0})});
        d.update(0, 3);
        CHECK(feq(d.at<float>(2)[0], 7) && feq(d.at<float>(2)[1], 3) && feq(d.at<float>(2)[2], 2));
    }
    {
        Direct d(variantOf("Subtract", {{"a", PT::Float2}, {"b", PT::Float2}}),
                 {vec<Vector2>({{10, 8}, {5, 3}}), vec<Vector2>({{3, 2}, {2, 1}}), vec<Vector2>({{}, {}})});
        d.update(0, 2);
        CHECK(veq(d.at<Vector2>(2)[0], {7, 6}) && veq(d.at<Vector2>(2)[1], {3, 2}));
    }
    {
        Direct d(variantOf("Subtract", {{"a", PT::Float3}, {"b", PT::Float3}}),
                 {vec<Vector3>({{10, 8, 6}}), vec<Vector3>({{3, 2, 1}}), vec<Vector3>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(2)[0], {7, 6, 5}));
    }
    {
        Direct d(variantOf("Subtract", {{"a", PT::Float4}, {"b", PT::Float4}}),
                 {vec<Vector4>({{10, 8, 6, 4}}), vec<Vector4>({{3, 2, 1, 2}}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(2)[0], {7, 6, 5, 2}));
    }
    // Multiply: every one of the 10 variants.
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float}, {"b", PT::Float}}), {vec<float>({2, 5, -3}), vec<float>({3, 2, 4}), vec<float>({0, 0, 0})});
        d.update(0, 3);
        CHECK(feq(d.at<float>(2)[0], 6) && feq(d.at<float>(2)[1], 10) && feq(d.at<float>(2)[2], -12));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float2}, {"b", PT::Float2}}), {vec<Vector2>({{2, 3}}), vec<Vector2>({{4, 5}}), vec<Vector2>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector2>(2)[0], {8, 15}));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float3}, {"b", PT::Float3}}), {vec<Vector3>({{2, 3, 4}}), vec<Vector3>({{5, 6, 7}}), vec<Vector3>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(2)[0], {10, 18, 28}));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float4}, {"b", PT::Float4}}),
                 {vec<Vector4>({{2, 3, 4, 5}}), vec<Vector4>({{6, 7, 8, 9}}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(2)[0], {12, 21, 32, 45}));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float}, {"b", PT::Float2}, {"product", PT::Float2}}),
                 {vec<float>({2}), vec<Vector2>({{3, 4}}), vec<Vector2>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector2>(2)[0], {6, 8}));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float}, {"b", PT::Float3}, {"product", PT::Float3}}),
                 {vec<float>({2}), vec<Vector3>({{3, 4, 5}}), vec<Vector3>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(2)[0], {6, 8, 10}));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float}, {"b", PT::Float4}, {"product", PT::Float4}}),
                 {vec<float>({2}), vec<Vector4>({{3, 4, 5, 6}}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(2)[0], {6, 8, 10, 12}));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float2}, {"b", PT::Float}, {"product", PT::Float2}}),
                 {vec<Vector2>({{3, 4}}), vec<float>({2}), vec<Vector2>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector2>(2)[0], {6, 8}));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float3}, {"b", PT::Float}, {"product", PT::Float3}}),
                 {vec<Vector3>({{3, 4, 5}}), vec<float>({2}), vec<Vector3>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(2)[0], {6, 8, 10}));
    }
    {
        Direct d(variantOf("Multiply", {{"a", PT::Float4}, {"b", PT::Float}, {"product", PT::Float4}}),
                 {vec<Vector4>({{3, 4, 5, 6}}), vec<float>({2}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(2)[0], {6, 8, 10, 12}));
    }
    // Divide: 7 variants (no number / vector).
    {
        Direct d(variantOf("Divide", {{"a", PT::Float}, {"b", PT::Float}}), {vec<float>({10, 15, -20}), vec<float>({2, 3, 4}), vec<float>({0, 0, 0})});
        d.update(0, 3);
        CHECK(feq(d.at<float>(2)[0], 5) && feq(d.at<float>(2)[1], 5) && feq(d.at<float>(2)[2], -5));
    }
    {
        Direct d(variantOf("Divide", {{"a", PT::Float2}, {"b", PT::Float2}}), {vec<Vector2>({{12, 15}}), vec<Vector2>({{3, 5}}), vec<Vector2>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector2>(2)[0], {4, 3}));
    }
    {
        Direct d(variantOf("Divide", {{"a", PT::Float3}, {"b", PT::Float3}}),
                 {vec<Vector3>({{12, 15, 20}}), vec<Vector3>({{3, 5, 4}}), vec<Vector3>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(2)[0], {4, 3, 5}));
    }
    {
        Direct d(variantOf("Divide", {{"a", PT::Float4}, {"b", PT::Float4}}),
                 {vec<Vector4>({{12, 15, 20, 24}}), vec<Vector4>({{3, 5, 4, 6}}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(2)[0], {4, 3, 5, 4}));
    }
    {
        Direct d(variantOf("Divide", {{"a", PT::Float2}, {"b", PT::Float}}), {vec<Vector2>({{12, 16}}), vec<float>({4}), vec<Vector2>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector2>(2)[0], {3, 4}));
    }
    {
        Direct d(variantOf("Divide", {{"a", PT::Float3}, {"b", PT::Float}}), {vec<Vector3>({{12, 16, 20}}), vec<float>({4}), vec<Vector3>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(2)[0], {3, 4, 5}));
    }
    {
        Direct d(variantOf("Divide", {{"a", PT::Float4}, {"b", PT::Float}}),
                 {vec<Vector4>({{12, 16, 20, 24}}), vec<float>({4}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(2)[0], {3, 4, 5, 6}));
    }
    CHECK(variantOf("Divide", {{"a", PT::Float}, {"b", PT::Float3}}) == nullptr);
}

// ---- Clamp / Min / Max / Invert / rounding ----------------------------------------------------------------------

void testRangeComponents() {
    for (const char* c : {"Clamp", "Min", "Max", "Invert"}) {
        CHECK_MSG(variantCount(c) == 4, c);
    }
    {
        Direct d(variantOf("Clamp", {{"value", PT::Float}}),
                 {vec<float>({-5, 5, 15}), vec<float>({0, 0, 0}), vec<float>({10, 10, 10}), vec<float>({0, 0, 0})});
        d.update(0, 3);
        CHECK(feq(d.at<float>(3)[0], 0) && feq(d.at<float>(3)[1], 5) && feq(d.at<float>(3)[2], 10));
    }
    {
        Direct d(variantOf("Clamp", {{"value", PT::Float2}}),
                 {vec<Vector2>({{-5, 15}, {5, 8}}), vec<float>({0, 0}), vec<float>({10, 10}), vec<Vector2>({{}, {}})});
        d.update(0, 2);
        CHECK(veq(d.at<Vector2>(3)[0], {0, 10}) && veq(d.at<Vector2>(3)[1], {5, 8}));
    }
    {
        Direct d(variantOf("Clamp", {{"value", PT::Float3}}),
                 {vec<Vector3>({{-5, 5, 15}}), vec<float>({0}), vec<float>({10}), vec<Vector3>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(3)[0], {0, 5, 10}));
    }
    {
        Direct d(variantOf("Clamp", {{"value", PT::Float4}}),
                 {vec<Vector4>({{-5, 5, 15, 12}}), vec<float>({0}), vec<float>({10}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(3)[0], {0, 5, 10, 10}));
    }
    {
        Direct d(variantOf("Min", {{"a", PT::Float}}), {vec<float>({5, 2, 10}), vec<float>({3, 8, 10}), vec<float>({0, 0, 0})});
        d.update(0, 3);
        CHECK(feq(d.at<float>(2)[0], 3) && feq(d.at<float>(2)[1], 2) && feq(d.at<float>(2)[2], 10));
        Direct m(variantOf("Max", {{"a", PT::Float}}), {vec<float>({5, 2, 10}), vec<float>({3, 8, 10}), vec<float>({0, 0, 0})});
        m.update(0, 3);
        CHECK(feq(m.at<float>(2)[0], 5) && feq(m.at<float>(2)[1], 8) && feq(m.at<float>(2)[2], 10));
    }
    {
        Direct d(variantOf("Min", {{"a", PT::Float2}}), {vec<Vector2>({{5, 2}}), vec<Vector2>({{3, 8}}), vec<Vector2>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector2>(2)[0], {3, 2}));
        Direct m(variantOf("Max", {{"a", PT::Float2}}), {vec<Vector2>({{5, 2}}), vec<Vector2>({{3, 8}}), vec<Vector2>({{}})});
        m.update(0, 1);
        CHECK(veq(m.at<Vector2>(2)[0], {5, 8}));
    }
    {
        Direct d(variantOf("Min", {{"a", PT::Float3}}), {vec<Vector3>({{5, 2, 10}}), vec<Vector3>({{3, 8, 10}}), vec<Vector3>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(2)[0], {3, 2, 10}));
        Direct m(variantOf("Max", {{"a", PT::Float3}}), {vec<Vector3>({{5, 2, 10}}), vec<Vector3>({{3, 8, 10}}), vec<Vector3>({{}})});
        m.update(0, 1);
        CHECK(veq(m.at<Vector3>(2)[0], {5, 8, 10}));
    }
    {
        Direct d(variantOf("Min", {{"a", PT::Float4}}), {vec<Vector4>({{5, 2, 10, 1}}), vec<Vector4>({{3, 8, 10, 2}}), vec<Vector4>({{}})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector4>(2)[0], {3, 2, 10, 1}));
        Direct m(variantOf("Max", {{"a", PT::Float4}}), {vec<Vector4>({{5, 2, 10, 1}}), vec<Vector4>({{3, 8, 10, 2}}), vec<Vector4>({{}})});
        m.update(0, 1);
        CHECK(veq(m.at<Vector4>(2)[0], {5, 8, 10, 2}));
    }
    {
        Direct f(specOf("Floor"), {vec<float>({2.7f, -2.7f, 5.0f}), vec<float>({0, 0, 0})});
        f.update(0, 3);
        CHECK(feq(f.at<float>(1)[0], 2) && feq(f.at<float>(1)[1], -3) && feq(f.at<float>(1)[2], 5));
        Direct c(specOf("Ceil"), {vec<float>({2.3f, -2.3f, 5.0f}), vec<float>({0, 0, 0})});
        c.update(0, 3);
        CHECK(feq(c.at<float>(1)[0], 3) && feq(c.at<float>(1)[1], -2) && feq(c.at<float>(1)[2], 5));
        Direct r(specOf("Round"), {vec<float>({2.3f, 2.7f, -2.3f}), vec<float>({0, 0, 0})});
        r.update(0, 3);
        CHECK(feq(r.at<float>(1)[0], 2) && feq(r.at<float>(1)[1], 3) && feq(r.at<float>(1)[2], -2));
    }
    {
        Direct d(variantOf("Invert", {{"input", PT::Float}}), {vec<float>({0.2f, 0.8f, 1.0f}), vec<float>({0, 0, 0})});
        d.update(0, 3);
        CHECK(feq(d.at<float>(1)[0], 0.8f) && feq(d.at<float>(1)[1], 0.2f) && feq(d.at<float>(1)[2], 0.0f));
        Direct d2(variantOf("Invert", {{"input", PT::Float2}}), {vec<Vector2>({{0.2f, 0.8f}, {0, 1}}), vec<Vector2>({{}, {}})});
        d2.update(0, 2);
        CHECK(veq(d2.at<Vector2>(1)[0], {0.8f, 0.2f}) && veq(d2.at<Vector2>(1)[1], {1, 0}));
        Direct d3(variantOf("Invert", {{"input", PT::Float3}}), {vec<Vector3>({{0.2f, 0.5f, 0.8f}}), vec<Vector3>({{}})});
        d3.update(0, 1);
        CHECK(veq(d3.at<Vector3>(1)[0], {0.8f, 0.5f, 0.2f}));
        Direct d4(variantOf("Invert", {{"input", PT::Float4}}), {vec<Vector4>({{0.2f, 0.4f, 0.6f, 0.8f}}), vec<Vector4>({{}})});
        d4.update(0, 1);
        CHECK(veq(d4.at<Vector4>(1)[0], {0.8f, 0.6f, 0.4f, 0.2f}));
    }
}

// ---- Comparisons and booleans -------------------------------------------------------------------------------------

void testComparisons() {
    CHECK(variantCount("EqualTo") == 4);
    {
        Direct d(variantOf("EqualTo", {{"a", PT::Float}, {"b", PT::Float}}),
                 {vec<float>({5, 3, 2, 1}), vec<float>({5, 4, 2, 1.05f}), vec<float>({0.00001f, 0.00001f, 0.00001f, 0.1f}),
                  vec<U>({0, 0, 0, 0})});
        d.update(0, 4);
        const auto& r = d.at<U>(3);
        CHECK(r[0] == 1 && r[1] == 0 && r[2] == 1 && r[3] == 1);
        Direct t(variantOf("EqualTo", {{"a", PT::Float}, {"b", PT::Float}}),
                 {vec<float>({0, 0}), vec<float>({0.09f, 0.11f}), vec<float>({0.1f, 0.1f}), vec<U>({0, 0})});
        t.update(0, 2);
        CHECK(t.at<U>(3)[0] == 1 && t.at<U>(3)[1] == 0);
    }
    {
        Direct d(variantOf("EqualTo", {{"a", PT::Float2}, {"b", PT::Float2}}),
                 {vec<Vector2>({{1, 2}, {3, 4}, {0, 0}}), vec<Vector2>({{1, 2}, {3, 5}, {0.05f, 0.05f}}),
                  vec<float>({0.00001f, 0.00001f, 0.1f}), vec<U>({0, 0, 0})});
        d.update(0, 3);
        CHECK(d.at<U>(3)[0] == 1 && d.at<U>(3)[1] == 0 && d.at<U>(3)[2] == 1);
    }
    {
        Direct d(variantOf("EqualTo", {{"a", PT::Float3}, {"b", PT::Float3}}),
                 {vec<Vector3>({{1, 2, 3}, {4, 5, 6}}), vec<Vector3>({{1, 2, 3}, {4, 5, 7}}), vec<float>({0.00001f, 0.00001f}), vec<U>({0, 0})});
        d.update(0, 2);
        CHECK(d.at<U>(3)[0] == 1 && d.at<U>(3)[1] == 0);
        Direct d4(variantOf("EqualTo", {{"a", PT::Float4}, {"b", PT::Float4}}),
                  {vec<Vector4>({{1, 2, 3, 4}}), vec<Vector4>({{1, 2, 3, 4}}), vec<float>({0.00001f}), vec<U>({0})});
        d4.update(0, 1);
        CHECK(d4.at<U>(3)[0] == 1);
    }
    {
        Direct lt(specOf("LessThan"), {vec<float>({3, 5, 5}), vec<float>({5, 3, 5}), vec<U>({0, 0, 0})});
        lt.update(0, 3);
        CHECK(lt.at<U>(2)[0] == 1 && lt.at<U>(2)[1] == 0 && lt.at<U>(2)[2] == 0);
        Direct gt(specOf("GreaterThan"), {vec<float>({5, 3, 5}), vec<float>({3, 5, 5}), vec<U>({0, 0, 0})});
        gt.update(0, 3);
        CHECK(gt.at<U>(2)[0] == 1 && gt.at<U>(2)[1] == 0 && gt.at<U>(2)[2] == 0);
    }
    {
        Direct b(specOf("Between"), {vec<float>({5, 0, 15, 10, 5, 5, 5}), vec<float>({0, 0, 0, 0, 5, 10, 0}),
                                     vec<float>({10, 10, 10, 10, 5, 5, 10}), vec<U>({0, 0, 0, 0, 0, 0, 0})});
        b.update(0, 7);
        const auto& r = b.at<U>(3);
        CHECK(r[0] == 1 && r[1] == 1 && r[2] == 0 && r[3] == 1 && r[4] == 1 && r[5] == 0 && r[6] == 1);
    }
    {
        Direct a(specOf("BoolAnd"), {vec<U>({1, 1, 0, 0}), vec<U>({1, 0, 1, 0}), vec<U>({0, 0, 0, 0})});
        a.update(0, 4);
        CHECK(a.at<U>(2) == std::vector<U>({1, 0, 0, 0}));
        Direct o(specOf("BoolOr"), {vec<U>({1, 1, 0, 0}), vec<U>({1, 0, 1, 0}), vec<U>({0, 0, 0, 0})});
        o.update(0, 4);
        CHECK(o.at<U>(2) == std::vector<U>({1, 1, 1, 0}));
        Direct n(specOf("BoolNot"), {vec<U>({1, 0, 1, 0}), vec<U>({0, 0, 0, 0})});
        n.update(0, 4);
        CHECK(n.at<U>(1) == std::vector<U>({0, 1, 0, 1}));
    }
}

// ---- Vectors -------------------------------------------------------------------------------------------------------

void testVectors() {
    {
        Direct c(specOf("ComposeVector2"), {vec<float>({1, 2}), vec<float>({3, 4}), vec<Vector2>({{}, {}})});
        c.update(0, 2);
        CHECK(veq(c.at<Vector2>(2)[0], {1, 3}) && veq(c.at<Vector2>(2)[1], {2, 4}));
        Direct c3(specOf("ComposeVector3"), {vec<float>({1, -2}), vec<float>({3, 4}), vec<float>({5, 6}), vec<Vector3>({{}, {}})});
        c3.update(0, 2);
        CHECK(veq(c3.at<Vector3>(3)[0], {1, 3, 5}) && veq(c3.at<Vector3>(3)[1], {-2, 4, 6}));
        Direct c4(specOf("ComposeVector4"),
                  {vec<float>({1, 5}), vec<float>({2, 6}), vec<float>({3, 7}), vec<float>({4, 8}), vec<Vector4>({{}, {}})});
        c4.update(0, 2);
        CHECK(veq(c4.at<Vector4>(4)[0], {1, 2, 3, 4}) && veq(c4.at<Vector4>(4)[1], {5, 6, 7, 8}));
    }
    {
        Direct d(specOf("DecomposeVector2"), {vec<Vector2>({{1, 2}, {3, 4}}), vec<float>({0, 0}), vec<float>({0, 0})});
        d.update(0, 2);
        CHECK(feq(d.at<float>(1)[0], 1) && feq(d.at<float>(2)[0], 2) && feq(d.at<float>(1)[1], 3) && feq(d.at<float>(2)[1], 4));
        Direct d3(specOf("DecomposeVector3"), {vec<Vector3>({{1, 2, 3}}), vec<float>({0}), vec<float>({0}), vec<float>({0})});
        d3.update(0, 1);
        CHECK(feq(d3.at<float>(1)[0], 1) && feq(d3.at<float>(2)[0], 2) && feq(d3.at<float>(3)[0], 3));
        Direct d4(specOf("DecomposeVector4"), {vec<Vector4>({{1, 2, 3, 4}}), vec<float>({0}), vec<float>({0}), vec<float>({0}), vec<float>({0})});
        d4.update(0, 1);
        CHECK(feq(d4.at<float>(1)[0], 1) && feq(d4.at<float>(2)[0], 2) && feq(d4.at<float>(3)[0], 3) && feq(d4.at<float>(4)[0], 4));
    }
    CHECK(variantCount("VectorLength") == 3);
    CHECK(variantCount("Normalize") == 3);
    {
        Direct l(variantOf("VectorLength", {{"input", PT::Float2}}), {vec<Vector2>({{3, 4}, {1, 0}}), vec<float>({0, 0})});
        l.update(0, 2);
        CHECK(feq(l.at<float>(1)[0], 5) && feq(l.at<float>(1)[1], 1));
        Direct l3(variantOf("VectorLength", {{"input", PT::Float3}}), {vec<Vector3>({{3, 4, 0}, {1, 0, 0}}), vec<float>({0, 0})});
        l3.update(0, 2);
        CHECK(feq(l3.at<float>(1)[0], 5) && feq(l3.at<float>(1)[1], 1));
        Direct l4(variantOf("VectorLength", {{"input", PT::Float4}}), {vec<Vector4>({{0, 0, 3, 4}}), vec<float>({0})});
        l4.update(0, 1);
        CHECK(feq(l4.at<float>(1)[0], 5));
    }
    {
        Direct n(variantOf("Normalize", {{"input", PT::Float2}}), {vec<Vector2>({{3, 4}, {5, 0}, {0, 0}}), vec<Vector2>({{}, {}, {}})});
        n.update(0, 3);
        CHECK(veq(n.at<Vector2>(1)[0], {0.6f, 0.8f}) && veq(n.at<Vector2>(1)[1], {1, 0}) && veq(n.at<Vector2>(1)[2], {0, 1}));
        Direct n3(variantOf("Normalize", {{"input", PT::Float3}}), {vec<Vector3>({{3, 4, 0}, {0, 5, 0}, {0, 0, 0}}), vec<Vector3>({{}, {}, {}})});
        n3.update(0, 3);
        CHECK(veq(n3.at<Vector3>(1)[0], {0.6f, 0.8f, 0}) && veq(n3.at<Vector3>(1)[1], {0, 1, 0}) &&
              veq(n3.at<Vector3>(1)[2], {0, 0, 1}));
        Direct n4(variantOf("Normalize", {{"input", PT::Float4}}), {vec<Vector4>({{0, 3, 0, 4}, {0, 0, 0, 0}}), vec<Vector4>({{}, {}})});
        n4.update(0, 2);
        CHECK(veq(n4.at<Vector4>(1)[0], {0, 0.6f, 0, 0.8f}) && veq(n4.at<Vector4>(1)[1], {0, 0, 0, 1}));
    }
}

// ---- Stateful -----------------------------------------------------------------------------------------------------

void testStateful() {
    {
        // Toggle: 3 instances, initialize from Starting State, then flip on triggers (upstream testToggle).
        Direct t(specOf("Toggle"), {vec<U>({1, 0, 1}), vec<U>({0, 0, 1}), vec<U>({0, 0, 0})});
        t.initialize(3);
        auto& isOn = t.at<U>(2);
        auto& trigger = t.at<U>(0);
        CHECK(isOn == std::vector<U>({0, 0, 1}));
        t.update(0, 3);
        CHECK(isOn == std::vector<U>({1, 0, 0}));
        t.update(0, 3);
        CHECK(isOn == std::vector<U>({0, 0, 1}));
        trigger = {0, 1, 0};
        t.update(0, 3);
        CHECK(isOn == std::vector<U>({0, 1, 1}));
        t.update(1, 2);
        CHECK(isOn[1] == 0);
        CHECK(specOf("Toggle")->properties[2].usdPropertyName == "outputs:isOn");
    }
    {
        // Counter (upstream testCounter).
        Direct c(specOf("Counter"), {vec<U>({1, 0, 1, 1, 1, 1, 1}), vec<float>({1, 1, 2.5f, -1.5f, 10, 1, 5}),
                                     vec<float>({0, 0, 0, 0, 0, 100, -50}), vec<float>({0, 0, 0, 0, 0, 0, 0}),
                                     vec<float>({0, 0, 0, 0, 0, 0, 0})});
        c.initialize(7);
        auto& v = c.at<float>(4);
        auto& inc = c.at<U>(0);
        auto expect = [&](std::initializer_list<float> e) {
            std::size_t i = 0;
            bool ok = true;
            for (float x : e) {
                ok = ok && feq(v[i++], x);
            }
            return ok;
        };
        CHECK(expect({0, 0, 0, 0, 0, 100, -50}));
        c.update(0, 7);
        CHECK(expect({1, 0, 2.5f, -1.5f, 10, 101, -45}));
        c.update(0, 7);
        CHECK(expect({2, 0, 5, -3, 20, 102, -40}));
        inc[4] = 0;
        c.update(0, 7);
        CHECK(expect({3, 0, 7.5f, -4.5f, 20, 103, -35}));
        c.update(0, 7);
        CHECK(expect({4, 0, 10, -6, 20, 104, -30}));
        inc[4] = 1;
        c.update(0, 7);
        CHECK(expect({5, 0, 12.5f, -7.5f, 30, 105, -25}));
    }
    {
        // CountToggles (upstream testCountToggles).
        Direct c(specOf("CountToggles"), {vec<U>({0, 0, 0, 0}), vec<float>({0, 3, 0, 0}), vec<U>({0, 0, 0, 0}), vec<float>({0, 0, 0, 0})});
        auto& value = c.at<U>(0);
        auto& count = c.at<float>(3);
        c.update(0, 4);
        CHECK(feq(count[0], 0) && feq(count[1], 0));
        value[0] = 1;
        value[1] = 1;
        c.update(0, 4);
        CHECK(feq(count[0], 1) && feq(count[1], 1) && feq(count[2], 0) && feq(count[3], 0));
        value[0] = 0;
        value[1] = 0;
        value[3] = 1;
        c.update(0, 4);
        CHECK(feq(count[0], 1) && feq(count[1], 1) && feq(count[2], 0) && feq(count[3], 1));
        value[0] = 1;
        value[1] = 1;
        c.update(0, 4);
        CHECK(feq(count[0], 2) && feq(count[1], 2) && feq(count[3], 1));
        value[0] = 0;
        value[1] = 0;
        c.update(0, 4);
        CHECK(feq(count[0], 2) && feq(count[1], 2));
        value[0] = 1;
        value[1] = 1;
        c.update(0, 4);
        CHECK(feq(count[0], 3) && feq(count[1], 0));
        value[0] = 0;
        value[1] = 0;
        c.update(0, 4);
        value[0] = 1;
        value[1] = 1;
        c.update(0, 4);
        CHECK(feq(count[0], 4) && feq(count[1], 1));
        // The state's default is true (upstream): an input that starts true is not an edge.
        CHECK(std::get<std::uint32_t>(specOf("CountToggles")->properties[2].defaultValue) == 1);
    }
    // ConditionallyStore / PreviousFrameValue / Select: every Any variant.
    for (const char* c : {"ConditionallyStore", "PreviousFrameValue", "Select"}) {
        CHECK_MSG(variantCount(c) == 9, c);
    }
    {
        Direct s(variantOf("ConditionallyStore", {{"input", PT::Float}}),
                 {vec<U>({1, 0, 1}), vec<float>({10, 20, 30}), vec<float>({0, 0, 0}), vec<float>({0, 0, 0})});
        s.update(0, 3);
        CHECK(feq(s.at<float>(3)[0], 10) && feq(s.at<float>(3)[1], 0) && feq(s.at<float>(3)[2], 30));
        s.at<U>(0) = {0, 0, 0};
        s.at<float>(1) = {11, 21, 31};
        s.update(0, 3);
        CHECK(feq(s.at<float>(3)[0], 10) && feq(s.at<float>(3)[2], 30)); // held
        Direct h(variantOf("ConditionallyStore", {{"input", PT::Hash}}), {vec<U>({1}), vec<std::uint64_t>({0xDEADBEEFull}),
                                                                           vec<std::uint64_t>({0}), vec<std::uint64_t>({0})});
        h.update(0, 1);
        CHECK(h.at<std::uint64_t>(3)[0] == 0xDEADBEEFull);
        Direct p(variantOf("ConditionallyStore", {{"input", PT::Prim}}),
                 {vec<U>({1}), vec<PrimTarget>({PrimTarget{5, 500}}), vec<PrimTarget>({PrimTarget{}}), vec<PrimTarget>({PrimTarget{}})});
        p.update(0, 1);
        CHECK(p.at<PrimTarget>(3)[0] == (PrimTarget{5, 500}));
        Direct str(variantOf("ConditionallyStore", {{"input", PT::String}}),
                   {vec<U>({1}), vec<std::string>({"test"}), vec<std::string>({""}), vec<std::string>({""})});
        str.update(0, 1);
        CHECK(str.at<std::string>(3)[0] == "test");
    }
    {
        Direct p(variantOf("PreviousFrameValue", {{"input", PT::Float}}), {vec<float>({10}), vec<float>({0}), vec<float>({0})});
        p.update(0, 1);
        CHECK(feq(p.at<float>(2)[0], 0));
        p.at<float>(0)[0] = 20;
        p.update(0, 1);
        CHECK(feq(p.at<float>(2)[0], 10));
        p.at<float>(0)[0] = 30;
        p.update(0, 1);
        CHECK(feq(p.at<float>(2)[0], 20));
        Direct b(variantOf("PreviousFrameValue", {{"input", PT::Bool}}), {vec<U>({1}), vec<U>({0}), vec<U>({0})});
        b.update(0, 1);
        CHECK(b.at<U>(2)[0] == 0);
        b.at<U>(0)[0] = 0;
        b.update(0, 1);
        CHECK(b.at<U>(2)[0] == 1);
        Direct v(variantOf("PreviousFrameValue", {{"input", PT::Float3}}), {vec<Vector3>({{1, 2, 3}}), vec<Vector3>({{}}), vec<Vector3>({{}})});
        v.update(0, 1);
        v.at<Vector3>(0)[0] = Vector3(4, 5, 6);
        v.update(0, 1);
        CHECK(veq(v.at<Vector3>(2)[0], {1, 2, 3}));
        Direct s(variantOf("PreviousFrameValue", {{"input", PT::String}}), {vec<std::string>({"new"}), vec<std::string>({""}), vec<std::string>({""})});
        s.update(0, 1);
        s.at<std::string>(0)[0] = "newer";
        s.update(0, 1);
        CHECK(s.at<std::string>(2)[0] == "new");
    }
    {
        Direct s(variantOf("Select", {{"inputA", PT::Float}}),
                 {vec<U>({1, 0, 1}), vec<float>({10, 20, 30}), vec<float>({5, 15, 25}), vec<float>({0, 0, 0})});
        s.update(0, 3);
        CHECK(feq(s.at<float>(3)[0], 10) && feq(s.at<float>(3)[1], 15) && feq(s.at<float>(3)[2], 30));
        Direct e(variantOf("Select", {{"inputA", PT::Enum}}), {vec<U>({1}), vec<U>({42}), vec<U>({99}), vec<U>({0})});
        e.update(0, 1);
        CHECK(e.at<U>(3)[0] == 42);
        Direct h(variantOf("Select", {{"inputA", PT::Hash}}), {vec<U>({0}), vec<std::uint64_t>({0x1234567890ABCDEFull}),
                                                                vec<std::uint64_t>({0xFEDCBA0987654321ull}), vec<std::uint64_t>({0})});
        h.update(0, 1);
        CHECK(h.at<std::uint64_t>(3)[0] == 0xFEDCBA0987654321ull);
        Direct v(variantOf("Select", {{"inputA", PT::Float3}}),
                 {vec<U>({1, 0}), vec<Vector3>({{1, 2, 3}, {7, 8, 9}}), vec<Vector3>({{4, 5, 6}, {10, 11, 12}}), vec<Vector3>({{}, {}})});
        v.update(0, 2);
        CHECK(veq(v.at<Vector3>(3)[0], {1, 2, 3}) && veq(v.at<Vector3>(3)[1], {10, 11, 12}));
        Direct str(variantOf("Select", {{"inputA", PT::String}}),
                   {vec<U>({0}), vec<std::string>({"hello"}), vec<std::string>({"world"}), vec<std::string>({""})});
        str.update(0, 1);
        CHECK(str.at<std::string>(3)[0] == "world");
    }
}

// ---- Remap / Loop --------------------------------------------------------------------------------------------------

void testRemapLoop() {
    CHECK(variantCount("Remap") == 4);
    CHECK(variantCount("Loop") == 4);
    // Remap's old name InterpolateFloat resolves to the same component.
    CHECK(getAnyComponentSpecVariant(componentTypeFromName(fullName("InterpolateFloat"))) == specOf("Remap"));
    auto remap = [](std::vector<float> value, std::vector<float> inMin, std::vector<float> inMax, std::vector<U> clampIn, U easing,
                    std::vector<U> reverse, float outMin, float outMax) {
        const std::size_t n = value.size();
        Direct d(variantOf("Remap", {{"outputMin", PT::Float}}),
                 {PropertyVector(value), PropertyVector(inMin), PropertyVector(inMax), PropertyVector(clampIn),
                  PropertyVector(std::vector<U>(n, easing)), PropertyVector(reverse), PropertyVector(std::vector<float>(n, outMin)),
                  PropertyVector(std::vector<float>(n, outMax)), PropertyVector(std::vector<float>(n, 0.0f))});
        d.update(0, n);
        return d.at<float>(8);
    };
    const std::vector<U> no5(5, 0u), yes5(5, 1u), no3(3, 0u);
    {
        const auto r = remap({0, 0.5f, 1, 1.5f, -0.5f}, {0, 0, 0, 0, 0}, {1, 1, 1, 1, 1}, {0, 0, 0, 0, 1}, 2, no5, 0, 100);
        CHECK(feq(r[0], 0) && feq(r[1], 25) && feq(r[2], 100) && feq(r[3], 225) && feq(r[4], 0));
    }
    {
        const auto r = remap({-10, 0, 5, 10, 20}, std::vector<float>(5, 0), std::vector<float>(5, 10), yes5, 2, no5, 0, 100);
        CHECK(feq(r[0], 0) && feq(r[1], 0) && feq(r[2], 25) && feq(r[3], 100) && feq(r[4], 100));
    }
    {
        const auto r = remap({25, 20, 15, 10, 5}, std::vector<float>(5, 20), std::vector<float>(5, 10), yes5, 1, no5, 0, 100);
        CHECK(feq(r[0], 0) && feq(r[1], 0) && feq(r[2], 12.5f) && feq(r[3], 100) && feq(r[4], 100));
    }
    {
        const auto r = remap({-5, 0, 5, 10, 15}, std::vector<float>(5, 0), std::vector<float>(5, 10), no5, 2, no5, 0, 100);
        CHECK(feq(r[0], 25) && feq(r[1], 0) && feq(r[2], 25) && feq(r[3], 100) && feq(r[4], 225));
    }
    {
        const auto r = remap({25, 20, 15, 10, 5}, std::vector<float>(5, 20), std::vector<float>(5, 10), no5, 1, no5, 0, 100);
        CHECK(feq(r[0], -12.5f) && feq(r[1], 0) && feq(r[2], 12.5f) && feq(r[3], 100) && feq(r[4], 337.5f));
    }
    {
        const auto r = remap({20, 15, 10}, {20, 20, 20}, {10, 10, 10}, no3, 3, no3, 0, 100);
        CHECK(feq(r[0], 0) && feq(r[1], 75) && feq(r[2], 100));
        const auto r2 = remap({0, 0.5f, 1}, {0, 0, 0}, {1, 1, 1}, no3, 2, no3, 100, 0);
        CHECK(feq(r2[0], 100) && feq(r2[1], 75) && feq(r2[2], 0));
        const auto r3 = remap({20, 15, 10}, {20, 20, 20}, {10, 10, 10}, no3, 1, no3, 100, 0);
        CHECK(feq(r3[0], 100) && feq(r3[1], 87.5f) && feq(r3[2], 0));
        const auto r4 = remap({0, 0.5f, 1}, {0, 0, 0}, {1, 1, 1}, no3, 2, {1, 1, 1}, 0, 100);
        CHECK(feq(r4[0], 0) && feq(r4[1], 75) && feq(r4[2], 100));
    }
    {
        // Vector ranges lerp per component.
        Direct d(variantOf("Remap", {{"outputMin", PT::Float2}}),
                 {vec<float>({0, 0.5f, 1}), vec<float>({0, 0, 0}), vec<float>({1, 1, 1}), vec<U>({0, 0, 0}), vec<U>({0, 0, 0}),
                  vec<U>({0, 0, 0}), vec<Vector2>({{0, 10}, {0, 10}, {0, 10}}), vec<Vector2>({{100, 20}, {100, 20}, {100, 20}}),
                  vec<Vector2>({{}, {}, {}})});
        d.update(0, 3);
        CHECK(veq(d.at<Vector2>(8)[0], {0, 10}) && veq(d.at<Vector2>(8)[1], {50, 15}) && veq(d.at<Vector2>(8)[2], {100, 20}));
    }
    // Easing curves at their anchor points (upstream applyInterpolation).
    for (U e = 0; e <= 8; ++e) {
        const float at0 = applyInterpolation(static_cast<InterpolationType>(e), 0.0f);
        const float at1 = applyInterpolation(static_cast<InterpolationType>(e), 1.0f);
        CHECK_MSG(feq(at0, 0.0f, 2e-3f) && feq(at1, 1.0f, 2e-3f), "easing " + std::to_string(e));
    }
    CHECK(feq(applyInterpolation(InterpolationType::Sine, 0.5f), 0.70710678f));
    CHECK(feq(applyInterpolation(InterpolationType::EaseInOut, 0.25f), 0.125f));
    CHECK(feq(applyInterpolation(InterpolationType::Exponential, 0.5f), 0.03125f));
    auto loop = [](std::vector<float> value, U type) {
        const std::size_t n = value.size();
        Direct d(variantOf("Loop", {{"value", PT::Float}}),
                 {PropertyVector(value), PropertyVector(std::vector<float>(n, 0.0f)), PropertyVector(std::vector<float>(n, 1.0f)),
                  PropertyVector(std::vector<U>(n, type)), PropertyVector(std::vector<float>(n, 0.0f)),
                  PropertyVector(std::vector<U>(n, 0u))});
        d.update(0, n);
        return std::make_pair(d.at<float>(4), d.at<U>(5));
    };
    {
        const auto [r, rev] = loop({0, 0.5f, 1, 1.5f, 2, 2.5f, -0.5f}, 0);
        CHECK(feq(r[0], 0) && feq(r[1], 0.5f) && feq(r[2], 0) && feq(r[3], 0.5f) && feq(r[4], 0) && feq(r[5], 0.5f) && feq(r[6], 0.5f));
        CHECK(rev == std::vector<U>(7, 0u));
    }
    {
        const auto [r, rev] = loop({0, 0.5f, 1, 1.5f, 2, 2.5f, 3, 3.5f, -0.5f}, 1);
        CHECK(feq(r[0], 0) && feq(r[1], 0.5f) && feq(r[2], 1) && feq(r[3], 0.5f) && feq(r[4], 0) && feq(r[5], 0.5f) && feq(r[6], 1) &&
              feq(r[7], 0.5f) && feq(r[8], 0.5f));
        CHECK(rev == std::vector<U>({0, 0, 1, 1, 0, 0, 1, 1, 1}));
    }
    {
        const auto [r, rev] = loop({-10, 0, 0.5f, 1, 10}, 2);
        CHECK(feq(r[0], -10) && feq(r[1], 0) && feq(r[2], 0.5f) && feq(r[3], 1) && feq(r[4], 10));
        const auto [c, crev] = loop({-10, 0, 0.5f, 1, 10}, 3);
        CHECK(feq(c[0], 0) && feq(c[1], 0) && feq(c[2], 0.5f) && feq(c[3], 1) && feq(c[4], 1));
        CHECK(crev == std::vector<U>(5, 0u));
    }
    {
        // Equal range: Min Range unless NoLoop.
        CHECK(applyLooping(5.0f, 2.0f, 2.0f, LoopingType::Loop).first == 2.0f);
        CHECK(applyLooping(5.0f, 2.0f, 2.0f, LoopingType::NoLoop).first == 5.0f);
        Direct d(variantOf("Loop", {{"value", PT::Float3}}),
                 {vec<Vector3>({{1.5f, 2.5f, -0.5f}}), vec<Vector3>({{0, 0, 0}}), vec<Vector3>({{1, 1, 1}}), vec<U>({1}),
                  vec<Vector3>({{}}), vec<U>({0})});
        d.update(0, 1);
        CHECK(veq(d.at<Vector3>(4)[0], {0.5f, 0.5f, 0.5f}) && d.at<U>(5)[0] == 1);
    }
}

// ---- Time based ---------------------------------------------------------------------------------------------------

void testTimeBased() {
    CHECK(variantCount("Smooth") == 4);
    CHECK(variantCount("Velocity") == 4);
    {
        Direct s(variantOf("Smooth", {{"input", PT::Float}}), {vec<float>({0}), vec<float>({10}), vec<U>({0}), vec<float>({0})});
        s.inputs.deltaTime = 1.0f / 60.0f;
        s.update(0, 1);
        CHECK(feq(s.at<float>(3)[0], 0));
        s.at<float>(0)[0] = 100;
        s.update(0, 1);
        const float after1 = s.at<float>(3)[0];
        CHECK(after1 > 0 && after1 < 100);
        // lerp(input, previous, exp2(-factor * dt)) exactly.
        CHECK(feq(after1, 100.0f + std::exp2(-10.0f / 60.0f) * (0.0f - 100.0f), 1e-4f));
        for (int i = 0; i < 100; ++i) {
            s.update(0, 1);
        }
        CHECK(feq(s.at<float>(3)[0], 100, 0.1f));
        Direct s3(variantOf("Smooth", {{"input", PT::Float3}}), {vec<Vector3>({{0, 0, 0}}), vec<float>({100}), vec<U>({0}), vec<Vector3>({{}})});
        s3.inputs.deltaTime = 1.0f / 60.0f;
        s3.update(0, 1);
        s3.at<Vector3>(0)[0] = Vector3(10, 20, 30);
        for (int i = 0; i < 10; ++i) {
            s3.update(0, 1);
        }
        CHECK(veq(s3.at<Vector3>(3)[0], {10, 20, 30}, 0.5f));
        Direct z(variantOf("Smooth", {{"input", PT::Float}}), {vec<float>({5}), vec<float>({0}), vec<U>({0}), vec<float>({0})});
        z.update(0, 1);
        z.at<float>(0)[0] = 50;
        z.update(0, 1);
        CHECK(feq(z.at<float>(3)[0], 5)); // factor 0: never changes
    }
    {
        Direct v(variantOf("Velocity", {{"input", PT::Float}}), {vec<float>({0}), vec<float>({0}), vec<float>({0})});
        v.inputs.deltaTime = 1.0f / 60.0f;
        v.update(0, 1);
        v.at<float>(0)[0] = 10;
        v.update(0, 1);
        CHECK(feq(v.at<float>(2)[0], 600.0f, 0.01f));
        Direct v2(variantOf("Velocity", {{"input", PT::Float2}}), {vec<Vector2>({{0, 0}}), vec<Vector2>({{0, 0}}), vec<Vector2>({{}})});
        v2.inputs.deltaTime = 1.0f / 60.0f;
        v2.update(0, 1);
        v2.at<Vector2>(0)[0] = Vector2(1, 2);
        v2.update(0, 1);
        CHECK(veq(v2.at<Vector2>(2)[0], {60, 120}, 0.01f));
    }
    {
        // Time: accumulates dt * max(0, speed); disabled + reset -> 0; disabled without reset -> held.
        Direct t(specOf("Time"), {vec<U>({1, 1, 0}), vec<U>({1, 1, 0}), vec<float>({1, 2, 1}), vec<float>({0, 0, 3}), vec<float>({0, 0, 0})});
        t.inputs.deltaTime = 0.25f;
        t.update(0, 3);
        t.update(0, 3);
        CHECK(feq(t.at<float>(4)[0], 0.5f) && feq(t.at<float>(4)[1], 1.0f) && feq(t.at<float>(4)[2], 3.0f));
        t.at<U>(0) = {0, 1, 0};
        t.at<float>(2)[1] = -5.0f;
        t.update(0, 3);
        CHECK(feq(t.at<float>(4)[0], 0.0f) && feq(t.at<float>(4)[1], 1.0f) && feq(t.at<float>(4)[2], 3.0f));
    }
}

// ---- Constants ------------------------------------------------------------------------------------------------------

void testConstants() {
    const std::vector<std::pair<const char*, PropertyType>> consts = {
        {"ConstAssetPath", PT::AssetPath}, {"ConstBool", PT::Bool},     {"ConstColor3", PT::Float3}, {"ConstColor4", PT::Float4},
        {"ConstFloat", PT::Float},         {"ConstFloat2", PT::Float2}, {"ConstFloat3", PT::Float3}, {"ConstFloat4", PT::Float4},
        {"ConstHash", PT::Hash},           {"ConstPrim", PT::Prim},     {"ConstString", PT::String}};
    for (const auto& [name, type] : consts) {
        const ComponentSpec* s = specOf(name);
        CHECK_MSG(s != nullptr && s->properties.size() == 1, name);
        if (s == nullptr) {
            continue;
        }
        const PropertySpec& p = s->properties[0];
        CHECK_MSG(p.type == type && p.isSettableOutput && p.usdPropertyName == "inputs:value" && s->categories == "Constants", name);
    }
    CHECK(specOf("ConstFloat")->uiName == "Constant Number");
    Direct c(specOf("ConstFloat"), {vec<float>({3.25f})});
    c.update(0, 1);
    CHECK(feq(c.at<float>(0)[0], 3.25f));
}

// ---- Sense ----------------------------------------------------------------------------------------------------------

PrimSnapshot meshAt(const Vector3& translation, const Vector3& scale, const Vector3& bmin, const Vector3& bmax) {
    PrimSnapshot s;
    s.kind = PrimSnapshot::Kind::Mesh;
    s.objectToWorld[0] = Vector4(scale.x, 0, 0, 0);
    s.objectToWorld[1] = Vector4(0, scale.y, 0, 0);
    s.objectToWorld[2] = Vector4(0, 0, scale.z, 0);
    s.objectToWorld[3] = Vector4(translation, 1);
    s.bounds.minPos = bmin;
    s.bounds.maxPos = bmax;
    return s;
}

void testSense() {
    {
        FrameInputs in;
        Single cam(specOf("Camera"), {}, in);
        CHECK(veq(cam.v3("forward"), {0, 0, -1}) && feq(cam.f("fovDegrees"), 60.0f, 1e-3f) && feq(cam.f("farPlane"), 1000));
        cam.inputs.camera.valid = true;
        cam.inputs.camera.position = Vector3(1, 2, 3);
        cam.inputs.camera.forward = Vector3(0, 0, 1);
        cam.inputs.camera.fovRadians = kPi / 2;
        cam.inputs.camera.aspectRatio = 1.5f;
        cam.update();
        CHECK(veq(cam.v3("position"), {1, 2, 3}) && veq(cam.v3("forward"), {0, 0, 1}) && feq(cam.f("fovDegrees"), 90.0f, 1e-3f) &&
              feq(cam.f("aspectRatio"), 1.5f));
    }
    {
        std::vector<std::uint32_t> keys;
        CHECK(parseVirtualKeys("CTRL, A", keys) && keys == std::vector<std::uint32_t>({0x11, 'A'}));
        CHECK(parseVirtualKeys(" shift ,space", keys) && keys == std::vector<std::uint32_t>({0x10, 0x20}));
        CHECK(!parseVirtualKeys("CTRL, NOPE", keys) && keys.empty());
        CHECK(!parseVirtualKeys("", keys));
        CHECK(virtualKeyName(0x70) == "F1" && virtualKeyName('Q') == "Q");
        Single k(specOf("KeyboardInput"), {{"keyString", std::string("CTRL, A")}});
        CHECK(!k.b("isPressed"));
        k.inputs.keysDown = {0x11, 'A'};
        k.inputs.keysPressed = {'A'};
        k.update();
        CHECK(k.b("isPressed") && k.b("wasJustPressed") && !k.b("wasClicked"));
        k.inputs.keysPressed.clear();
        k.update();
        CHECK(k.b("isPressed") && !k.b("wasJustPressed") && !k.b("wasClicked"));
        k.inputs.keysDown = {0x11};
        k.update();
        CHECK(!k.b("isPressed") && k.b("wasClicked"));
        k.update();
        CHECK(!k.b("wasClicked"));
        Single bad(specOf("KeyboardInput"), {{"keyString", std::string("NOT_A_KEY")}});
        CHECK(!bad.b("isPressed"));
    }
    {
        FrameInputs in;
        in.meshHashUsage[0xABCDull] = 3;
        in.textureHashUsage[0x1234ull] = 2;
        in.lightHashes = {0x77ull};
        in.fogHash = 0x99ull;
        Single m(specOf("MeshHashChecker"), {{"meshHash", PropertyValue(std::in_place_type<std::uint64_t>, 0xABCDull)}}, in);
        CHECK(m.b("isUsed") && feq(m.f("usageCount"), 3));
        m.inputs.meshHashUsage.clear();
        m.update();
        CHECK(!m.b("isUsed") && feq(m.f("usageCount"), 0));
        Single t(specOf("TextureHashChecker"), {{"textureHash", PropertyValue(std::in_place_type<std::uint64_t>, 0x1234ull)}}, in);
        CHECK(t.b("isUsed") && feq(t.f("usageCount"), 2));
        Single l(specOf("LightHashChecker"), {{"lightHash", PropertyValue(std::in_place_type<std::uint64_t>, 0x77ull)}}, in);
        CHECK(l.b("isUsed"));
        Single f(specOf("FogHashChecker"), {{"fogHash", PropertyValue(std::in_place_type<std::uint64_t>, 0x99ull)}}, in);
        CHECK(f.b("isMatch"));
        Single f2(specOf("FogHashChecker"), {{"fogHash", PropertyValue(std::in_place_type<std::uint64_t>, 0x98ull)}}, in);
        CHECK(!f2.b("isMatch"));
    }
    {
        // A unit box at (10, 0, 0), scaled 2x: object space [-1, 1]^3.
        FrameInputs in;
        in.prims[Single::kOwner] = {meshAt({10, 0, 0}, {2, 2, 2}, {-1, -1, -1}, {1, 1, 1}), PrimSnapshot{}};
        in.prims[Single::kOwner][1].kind = PrimSnapshot::Kind::Light;
        in.prims[Single::kOwner][1].lightPosition = Vector3(4, 5, 6);
        const PrimTarget target{0, PrimTarget::kInvalidInstanceId};
        Single p(specOf("MeshProximity"),
                 {{"target", target}, {"worldPosition", Vector3(16, 0, 0)}, {"inactiveDistance", 4.0f}, {"fullActivationDistance", 0.0f}},
                 in);
        // (16 - 10) / 2 = 3 in object space; 2 outside the box; strength (2 - 4) / (0 - 4) = 0.5.
        CHECK(feq(p.f("signedDistance"), 2.0f) && feq(p.f("activationStrength"), 0.5f));
        p.set("worldPosition", Vector3(10.5f, 0, 0));
        p.update();
        CHECK(feq(p.f("signedDistance"), -0.75f) && feq(p.f("activationStrength"), 1.0f));
        p.set("target", PrimTarget{});
        p.update();
        CHECK(p.f("signedDistance") == FLT_MAX && feq(p.f("activationStrength"), 0.0f));

        Single r(specOf("RayMeshIntersection"), {{"target", target}, {"rayOrigin", Vector3(0, 0, 0)}, {"rayDirection", Vector3(1, 0, 0)}}, in);
        CHECK(r.b("intersects"));
        r.set("rayDirection", Vector3(-1, 0, 0));
        r.update();
        CHECK(!r.b("intersects"));
        r.set("rayDirection", Vector3(1, 0.05f, 0));
        r.update();
        CHECK(r.b("intersects"));

        Single a(specOf("AngleToMesh"), {{"target", target}, {"worldPosition", Vector3(0, 0, 0)}, {"direction", Vector3(0, 0, 1)}}, in);
        CHECK(feq(a.f("angleDegrees"), 90.0f, 1e-3f) && veq(a.v3("directionToCentroid"), {1, 0, 0}));
        a.set("direction", Vector3(1, 1, 0));
        a.update();
        CHECK(feq(a.f("angleDegrees"), 45.0f, 1e-3f) && feq(a.f("angleRadians"), kPi / 4, 1e-5f));

        Single t(specOf("ReadTransform"), {{"target", target}}, in);
        CHECK(veq(t.v3("position"), {10, 0, 0}) && veq(t.v3("scale"), {2, 2, 2}) && veq(t.v4("rotation"), {0, 0, 0, 1}));
        t.set("target", PrimTarget{1, PrimTarget::kInvalidInstanceId});
        t.update();
        CHECK(veq(t.v3("position"), {4, 5, 6}) && veq(t.v3("scale"), {1, 1, 1}));
    }
    {
        // A 90 degree rotation about Y (basis x -> -z); upstream decomposeMatrix's quaternion for it.
        PrimSnapshot s = meshAt({1, 2, 3}, {1, 1, 1}, {-1, -1, -1}, {1, 1, 1});
        s.objectToWorld[0] = Vector4(0, 0, -1, 0);
        s.objectToWorld[2] = Vector4(1, 0, 0, 0);
        Vector3 pos, scale;
        Vector4 rot;
        decomposeMatrix(s.objectToWorld, pos, rot, scale);
        CHECK(veq(pos, {1, 2, 3}) && veq(scale, {1, 1, 1}) && veq(rot, {0, -0.70710678f, 0, 0.70710678f}));
        // Bones: world = objectToWorld * bone (DXVK order).
        PrimSnapshot skinned = meshAt({0, 0, 0}, {1, 1, 1}, {-1, -1, -1}, {1, 1, 1});
        Matrix4 bone;
        bone[3] = Vector4(0, 5, 0, 1);
        skinned.boneMatrices = {Matrix4(), bone};
        FrameInputs in;
        in.prims[Single::kOwner] = {skinned};
        Single b(specOf("ReadBoneTransform"), {{"target", PrimTarget{0, PrimTarget::kInvalidInstanceId}}, {"boneIndex", 0.6f}}, in);
        CHECK(veq(b.v3("position"), {0, 5, 0}));
        b.set("boneIndex", 5.0f);
        b.update();
        CHECK(veq(b.v3("position"), {0, 0, 0}) && veq(b.v3("scale"), {1, 1, 1}));
    }
    {
        const Matrix4 m = [] {
            Matrix4 x;
            x[0] = Vector4(2, 0, 0, 0);
            x[1] = Vector4(0, 0, 3, 0);
            x[2] = Vector4(0, -4, 0, 0);
            x[3] = Vector4(5, 6, 7, 1);
            return x;
        }();
        const Matrix4 id = multiply(m, inverse(m));
        bool identity = true;
        for (std::size_t i = 0; i < 4; ++i) {
            for (std::size_t j = 0; j < 4; ++j) {
                identity = identity && feq(id[i][j], i == j ? 1.0f : 0.0f);
            }
        }
        CHECK(identity);
        CHECK(veq(transform(m, Vector4(1, 1, 1, 1)).xyz(), {7, 2, 10}));
    }
}

// ---- Options ------------------------------------------------------------------------------------------------------

void testOptions(const fs::path& tmp) {
    namespace opt = fuse::relight::options;
    (void)TestOptions::flag;
    {
        Single rb(specOf("RtxOptionReadBool"), {{"optionName", std::string("rtx.logicTest.flag")}});
        CHECK(rb.b("value"));
        Single rn(specOf("RtxOptionReadNumber"), {{"optionName", std::string("rtx.logicTest.number")}});
        CHECK(feq(rn.f("value"), 1.5f));
        Single ri(specOf("RtxOptionReadNumber"), {{"optionName", std::string("rtx.logicTest.count")}});
        CHECK(feq(ri.f("value"), 7.0f));
        Single r2(specOf("RtxOptionReadVector2"), {{"optionName", std::string("rtx.logicTest.v2")}});
        CHECK(veq(std::get<Vector2>(r2.get("value")), {1, 2}));
        Single r3(specOf("RtxOptionReadVector3"), {{"optionName", std::string("rtx.logicTest.v3")}});
        CHECK(veq(r3.v3("value"), {1, 2, 3}));
        Single c3(specOf("RtxOptionReadColor3"), {{"optionName", std::string("rtx.logicTest.v3")}});
        CHECK(veq(c3.v3("value"), {1, 2, 3}));
        Single c4(specOf("RtxOptionReadColor4"), {{"optionName", std::string("rtx.logicTest.v4")}});
        CHECK(veq(c4.v4("value"), {1, 2, 3, 4}));
        Single missing(specOf("RtxOptionReadColor4"), {{"optionName", std::string("rtx.logicTest.nope")}});
        CHECK(veq(missing.v4("value"), {0, 0, 0, 1}));
        Single wrongType(specOf("RtxOptionReadBool"), {{"optionName", std::string("rtx.logicTest.number")}});
        CHECK(!wrongType.b("value"));
    }
    {
        const fs::path conf = tmp / "logic_test_layer.conf";
        {
            std::ofstream f(conf);
            f << "rtx.logicTest.flag = False\nrtx.logicTest.number = 3.5\n";
        }
        const std::string confPath = conf.generic_string();
        CHECK(TestOptions::flag() && feq(TestOptions::number(), 1.5f));
        FrameInputs in;
        // Two instances controlling the same layer (priority 20000): enabled if ANY requests it, MAX strength.
        Single a(specOf("RtxOptionLayerAction"),
                 {{"configPath", confPath}, {"enabled", kFalsePropertyValue}, {"priority", 20000.4f}, {"blendStrength", 0.5f}}, in);
        Single b(specOf("RtxOptionLayerAction"),
                 {{"configPath", confPath}, {"enabled", kFalsePropertyValue}, {"priority", 20000.0f}, {"blendStrength", 0.25f}}, in);
        Single sensor(specOf("RtxOptionLayerSensor"), {{"configPath", confPath}, {"priority", 20000.0f}}, in);
        const auto held = heldOptionLayers();
        CHECK(held.size() == 1 && held[0].priority == 20000 && held[0].references == 2);
        opt::OptionManager::applyPendingValues(nullptr, false);
        CHECK(TestOptions::flag() && feq(TestOptions::number(), 1.5f)); // disabled: the defaults
        sensor.update();
        CHECK(!sensor.b("isEnabled"));
        a.set("enabled", kTruePropertyValue);
        a.update();
        b.update();
        opt::OptionManager::applyPendingValues(nullptr, false);
        // Strength 0.5: floats blend (1.5 -> 3.5 halfway), bools apply above the 0.1 threshold.
        CHECK(!TestOptions::flag());
        CHECK_MSG(feq(TestOptions::number(), 2.5f, 1e-4f), std::to_string(TestOptions::number()));
        sensor.update();
        CHECK(sensor.b("isEnabled"));
        b.set("enabled", kTruePropertyValue);
        b.set("blendStrength", 1.0f);
        a.update();
        b.update();
        opt::OptionManager::applyPendingValues(nullptr, false);
        CHECK(feq(TestOptions::number(), 3.5f, 1e-4f)); // MAX of 0.5 and 1.0
        // Threshold above the strength: the bool no longer applies.
        a.set("blendStrength", 0.3f);
        a.set("blendThreshold", 0.4f);
        b.set("enabled", kFalsePropertyValue);
        a.update();
        b.update();
        opt::OptionManager::applyPendingValues(nullptr, false);
        CHECK(TestOptions::flag());
        CHECK(feq(TestOptions::number(), 1.5f + 0.3f * 2.0f, 1e-4f));
        // Changing the config path re-acquires; releasing every instance removes the layer and its values.
        a.set("configPath", std::string(""));
        a.update();
        CHECK(heldOptionLayers().size() == 1 && heldOptionLayers()[0].references == 1);
    }
    opt::OptionManager::applyPendingValues(nullptr, false);
    CHECK(heldOptionLayers().empty());
    CHECK(TestOptions::flag() && feq(TestOptions::number(), 1.5f));
    CHECK(opt::OptionManager::getLayer(opt::OptionLayerKey(20000, (tmp / "logic_test_layer.conf").generic_string())) == nullptr);
}

} // namespace

void testComponents() {
    registerAllComponents();
    const fs::path tmp = fs::absolute("rl_logic_tmp");
    fs::create_directories(tmp);
    testArithmetic();
    testRangeComponents();
    testComparisons();
    testVectors();
    testStateful();
    testRemapLoop();
    testTimeBased();
    testConstants();
    testSense();
    testOptions(tmp);
    takeLogMessages();
}

} // namespace rl_logic_test
