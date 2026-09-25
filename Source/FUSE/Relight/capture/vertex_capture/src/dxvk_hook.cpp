// FUSE Relight RL-1.6: the process-wide DXVK hand-over for vertex capture. See dxvk_hook.hpp.
#include <fuse/relight/capture/vertex_capture/dxvk_hook.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace fuse::relight::capture::vertex_capture::dxvk_hook {

namespace {

struct Registry {
    std::mutex mutex;
    std::atomic<bool> enabled{false};
    std::set<std::string> layouts; ///< shaders whose layout got the binding
    void* owner = nullptr;
    SubstituteFn fn = nullptr;
    Stats stats;
};

Registry& registry() {
    static Registry r;
    return r;
}

bool isD3D9VertexShader(const char* name) { return name && std::string_view(name).rfind("vs.", 0) == 0; }

void dump(const char* dir, const std::string& name, const char* suffix, const std::uint32_t* words, std::size_t count) {
    const std::string path = std::string(dir) + "/" + name + suffix;
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(words, sizeof(std::uint32_t), count, f);
        std::fclose(f);
    }
}

} // namespace

void enable() { registry().enabled.store(true); }

bool enabled() { return registry().enabled.load(); }

void setSubstitutor(void* owner, SubstituteFn fn) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.owner = owner;
    r.fn = fn;
}

void clearSubstitutor(void* owner) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.owner == owner) {
        r.owner = nullptr;
        r.fn = nullptr;
    }
}

Stats stats() {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.stats;
}

bool layoutBinding(const char* shaderName, std::uint32_t* set, std::uint32_t* binding, std::uint32_t* slot) {
    Registry& r = registry();
    if (!r.enabled.load() || !isD3D9VertexShader(shaderName)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        if (r.layouts.insert(shaderName).second) {
            ++r.stats.layouts;
        }
    }
    *set = kDescriptorSet;
    *binding = kBinding;
    *slot = kResourceSlot;
    return true;
}

bool substituteCode(const char* shaderName, const std::uint32_t* code, std::size_t words, std::uint32_t set,
                    std::uint32_t binding, std::vector<std::uint32_t>* out) {
    Registry& r = registry();
    if (!r.enabled.load() || !isD3D9VertexShader(shaderName) || !out) {
        return false;
    }
    // The substitutor runs under the registry lock: the dispatcher clears it (device destruction)
    // only after an offer in flight has returned.
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.fn || r.layouts.count(shaderName) == 0) {
        return false;
    }
    tap::ShaderModule module;
    module.spirv = code;
    module.wordCount = words;
    module.name = shaderName;
    module.captureSet = set;
    module.captureBinding = binding;
    out->clear();
    const bool ok = r.fn(r.owner, module, *out) && !out->empty();
    ++(ok ? r.stats.substituted : r.stats.declined);
    if (const char* dir = std::getenv("FUSE_RELIGHT_VC_DUMP"); dir && *dir) {
        dump(dir, shaderName, ".in.spv", code, words);
        if (ok) {
            dump(dir, shaderName, ".out.spv", out->data(), out->size());
        }
    }
    return ok;
}

} // namespace fuse::relight::capture::vertex_capture::dxvk_hook

// The hand-over points the FUSE-DXVK patches RL-1.6-01 / -02 declare inside namespace dxvk.
namespace dxvk { // fuse-lint-allow(namespace): hand-over points the FUSE-DXVK patches declare in dxvk::

bool fuseRelightVertexCaptureBinding(const char* shaderName, std::uint32_t* set, std::uint32_t* binding,
                                     std::uint32_t* slot) {
    return fuse::relight::capture::vertex_capture::dxvk_hook::layoutBinding(shaderName, set, binding, slot);
}

bool fuseRelightVertexCaptureCode(const char* shaderName, const std::uint32_t* code, std::size_t words,
                                  std::uint32_t set, std::uint32_t binding, std::vector<std::uint32_t>* out) {
    return fuse::relight::capture::vertex_capture::dxvk_hook::substituteCode(shaderName, code, words, set, binding, out);
}

} // namespace dxvk
