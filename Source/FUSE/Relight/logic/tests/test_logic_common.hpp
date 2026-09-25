// FUSE Relight RL-3.5: shared helpers of the Logic graph unit tests (rl_logic_unit).
#pragma once

#include <fuse/relight/logic/component_list.hpp>
#include <fuse/relight/logic/graph_batch.hpp>
#include <fuse/relight/logic/graph_types.hpp>
#include <fuse/relight/logic/logic_context.hpp>
#include <fuse/relight/options/option.hpp>

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rl_logic_test {

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++::rl_logic_test::g_checks;                                                               \
        if (!(cond)) {                                                                             \
            ++::rl_logic_test::g_failures;                                                         \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                                          \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                       \
    do {                                                                                           \
        ++::rl_logic_test::g_checks;                                                               \
        if (!(cond)) {                                                                             \
            ++::rl_logic_test::g_failures;                                                         \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s (%s)\n", __FILE__, __LINE__, #cond,      \
                         std::string(msg).c_str());                                                \
        }                                                                                          \
    } while (0)

using namespace fuse::relight::logic;
using PT = PropertyType;

inline bool feq(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }
inline bool veq(const Vector2& a, const Vector2& b, float eps = 1e-5f) { return feq(a.x, b.x, eps) && feq(a.y, b.y, eps); }
inline bool veq(const Vector3& a, const Vector3& b, float eps = 1e-5f) {
    return feq(a.x, b.x, eps) && feq(a.y, b.y, eps) && feq(a.z, b.z, eps);
}
inline bool veq(const Vector4& a, const Vector4& b, float eps = 1e-5f) {
    return feq(a.x, b.x, eps) && feq(a.y, b.y, eps) && feq(a.z, b.z, eps) && feq(a.w, b.w, eps);
}

inline std::string fullName(const std::string& cls) { return std::string(PropertySpec::kUsdNamePrefix) + cls; }

/// The variant of `cls` whose resolved types match `types` (nullptr when none).
const ComponentSpec* variantOf(const std::string& cls, const std::map<std::string, PropertyType>& types);
const ComponentSpec* specOf(const std::string& cls);
std::size_t variantCount(const std::string& cls);

/// Upstream's test pattern: a component batch over caller-owned property vectors (one per spec property, in spec
/// order), updated directly.
struct Direct {
    GraphBatch batch; ///< never initialized: stands in for upstream's MockGraphBatch
    FrameInputs inputs;
    std::vector<PropertyVector> props;
    std::unique_ptr<ComponentBatch> comp;
    const ComponentSpec* spec = nullptr;

    Direct(const ComponentSpec* s, std::vector<PropertyVector> p);
    void initialize(std::size_t count); ///< runs spec->initialize for instances [0, count)
    void update(std::size_t start, std::size_t end);
    template <typename T>
    std::vector<T>& at(std::size_t i) {
        return std::get<std::vector<T>>(props[i]);
    }
};

/// One component instance created through GraphManager (so Prim targets resolve against inputs.prims[owner]).
/// Values: the spec defaults, overridden by `values` (by property name).
struct Single {
    GraphManager manager;
    FrameInputs inputs;
    GraphInstance* instance = nullptr;
    const ComponentSpec* spec = nullptr;
    static constexpr std::uint64_t kOwner = 7;

    Single(const ComponentSpec* s, const std::map<std::string, PropertyValue>& values, const FrameInputs& in = {});
    ~Single();
    void update();
    PropertyValue get(const std::string& property) const;
    void set(const std::string& property, const PropertyValue& value);
    float f(const std::string& p) const { return std::get<float>(get(p)); }
    bool b(const std::string& p) const { return std::get<std::uint32_t>(get(p)) != 0; }
    Vector3 v3(const std::string& p) const { return std::get<Vector3>(get(p)); }
    Vector4 v4(const std::string& p) const { return std::get<Vector4>(get(p)); }
};

// Test groups (test_logic_components.cpp, test_logic_parser.cpp).
void testComponents();
void testParser();

} // namespace rl_logic_test
