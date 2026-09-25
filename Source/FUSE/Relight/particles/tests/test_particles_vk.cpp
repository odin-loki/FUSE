// FUSE Relight RL-3.6 Lavapipe gates (rl_particles_vk_*): the GLSL twin on the GPU against the CPU reference.
//
//   parity [--frames-in-flight N]   the fixture systems (particle_scenarios.hpp) run side by side on a CpuReference
//       manager and a Vulkan manager fed the same calls, with VK_LAYER_KHRONOS_validation + synchronization validation
//       (every warning or error fails the run). Integer state must be identical: per frame the ring counters of every
//       system (retirements come back from the GPU counters), per snapshot every particle's state flag, random seed
//       and time to live (bit for bit: they only go through correctly rounded, uncontracted f32 add / sub / mul) and
//       its uv rectangle (copied). Floats within the tolerance stated at kTolerance; colours within one unorm8 step.
//       Billboard vertices (the whole vertex pool, zero vertices included) against the CPU reference the same way.
//   zero_alloc   no heap allocation in steady state through the Vulkan backend (validation off: the layer is C++ and
//                allocates through operator new).
//
// Exit 77 = skip (no Vulkan backend in this build, no kernels, no ICD or validation layer).
#include "particle_scenarios.hpp"

#include <fuse/relight/particles/particle_vulkan.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <vulkan/vulkan.h>
#endif

namespace {
std::atomic<bool> g_countAllocs{false};
std::atomic<std::uint64_t> g_allocs{0};
} // namespace

// GCC may inline the replacement operators into callers and then report a false -Wmismatched-new-delete at -O2;
// replacement functions must not be inline anyway.
#if defined(__GNUC__)
#define RL_PT_NOINLINE __attribute__((noinline))
#else
#define RL_PT_NOINLINE
#endif

RL_PT_NOINLINE void* operator new(std::size_t n) {
    if (g_countAllocs.load(std::memory_order_relaxed)) {
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc();
}
RL_PT_NOINLINE void* operator new[](std::size_t n) { return operator new(n); }
RL_PT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
RL_PT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
RL_PT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
RL_PT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr int kSkip = 77;
int g_failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: no Vulkan backend in this build\n");
    return kSkip;
}

#else

namespace {

using namespace rl_particles_test;
using fuse::renderer::GpuAllocator;
using fuse::renderer::VulkanDevice;
using fuse::renderer::VulkanInstance;

// GPU vs CPU tolerance for positions, velocities, rotations and billboard vertices: |gpu - cpu| <= kTolerance x
// max(1, |cpu|) (cm / (cm/s) / rad). The two sides evaluate the same f32 expressions in the same order, the float
// chains are uncontracted (`precise`) and sine / cosine are a portable polynomial; what Vulkan still allows to differ
// is sqrt / inversesqrt / division (<= 2.5 ulp; exact on Lavapipe's LLVM), a few 1e-7 relative per frame, growing
// about linearly over the 150 frames (~1e-5). kTolerance = 1e-4 is the plan's GPU budget. On Lavapipe the measured
// error is 0 (bit-identical), which the gate prints.
// Turbulent systems are chaotic: the curl of the trilinear value noise has derivative jumps at lattice cells, so the
// separation of two trajectories that differ by one ulp grows by roughly force x dt x |grad curl| per frame and a
// trajectory tolerance would measure the chaos, not the port. Their trajectories are held to exact integer state
// (life, death, counts: independent of the positions) and their float state to kTolerance for one step from the
// CPU state (--one-step: the GPU pool is re-synchronized before every compared frame).
constexpr double kTolerance = 1e-4;

std::uint32_t g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_messages <= 20u) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n", data && data->pMessageIdName ? data->pMessageIdName : "(no id)",
                     data && data->pMessage ? data->pMessage : "");
    }
    return VK_FALSE;
}

bool layerAvailable(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    ~Context() {
        if (device) {
            device->waitIdle();
        }
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

int setup(Context& ctx, bool validation) {
    constexpr const char* kLayer = "VK_LAYER_KHRONOS_validation";
    if (validation) {
        if (!layerAvailable(kLayer)) {
            std::printf("SKIP: %s not installed\n", kLayer);
            return kSkip;
        }
        setEnv("VK_INSTANCE_LAYERS", kLayer);
        setEnv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
        setEnv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
        setEnv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK");
    } else {
        setEnv("VK_INSTANCE_LAYERS", "");
    }
    fuse::renderer::VulkanInstanceDesc desc{};
    desc.appName = "fuse_relight_particles_vk";
    desc.enableValidation = validation;
    ctx.instance = VulkanInstance::create(desc);
    if (!ctx.instance || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
        ctx.destroyMessenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
        if (create == nullptr) {
            std::fprintf(stderr, "FAIL: VK_EXT_debug_utils unavailable with validation\n");
            return 1;
        }
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = onMessage;
        create(vkInstance, &info, nullptr, &ctx.messenger);
    }
    ctx.device = VulkanDevice::create(*ctx.instance);
    if (!ctx.device || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (!ctx.allocator || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    return 0;
}

double relErr(double g, double c) { return std::fabs(g - c) / std::max(1.0, std::fabs(c)); }

struct Stats {
    double pos = 0, vel = 0, rot = 0, vtx = 0, uv = 0;
    std::uint32_t colorOff = 0, intMismatch = 0, compared = 0, vertices = 0;
};

void compareParticles(const std::vector<GpuParticle>& g, std::span<const GpuParticle> c, std::size_t begin, std::size_t end, Stats& st) {
    for (std::size_t i = begin; i < end && i < c.size(); ++i) {
        const GpuParticle& a = g[i];
        const GpuParticle& b = c[i];
        if (a.state != b.state || std::memcmp(&a.randSeed, &b.randSeed, 4) != 0 || std::memcmp(&a.timeToLive, &b.timeToLive, 4) != 0 ||
            std::memcmp(a.uvMinMax, b.uvMinMax, sizeof(a.uvMinMax)) != 0) {
            if (st.intMismatch < 5) {
                std::fprintf(stderr, "  particle %zu: state %u/%u seed %a/%a ttl %a/%a\n", i, a.state, b.state, a.randSeed, b.randSeed,
                             a.timeToLive, b.timeToLive);
            }
            ++st.intMismatch;
            continue;
        }
        if (b.state == kParticleDead) {
            continue;
        }
        ++st.compared;
        for (int k = 0; k < 3; ++k) {
            st.pos = std::max(st.pos, relErr(a.position[k], b.position[k]));
            st.vel = std::max(st.vel, relErr(a.velocity[k], b.velocity[k]));
        }
        st.rot = std::max(st.rot, relErr(a.rotation, b.rotation));
        for (int ch = 0; ch < 4; ++ch) {
            const int ga = static_cast<int>((a.color >> (8 * ch)) & 0xFF), cb = static_cast<int>((b.color >> (8 * ch)) & 0xFF);
            st.colorOff += std::abs(ga - cb) > 1 ? 1u : 0u;
        }
    }
}

void compareVertices(const std::vector<GpuParticleVertex>& g, std::span<const GpuParticleVertex> c, std::size_t begin, std::size_t end,
                     Stats& st) {
    for (std::size_t i = begin; i < end && i < c.size(); ++i) {
        const GpuParticleVertex& a = g[i];
        const GpuParticleVertex& b = c[i];
        const bool za = a.color == 0u && a.position[0] == 0.f && a.position[1] == 0.f && a.position[2] == 0.f;
        const bool zb = b.color == 0u && b.position[0] == 0.f && b.position[1] == 0.f && b.position[2] == 0.f;
        if (za != zb) {
            ++st.intMismatch; // culled on one side only
            continue;
        }
        if (zb) {
            continue;
        }
        ++st.vertices;
        for (int k = 0; k < 3; ++k) {
            st.vtx = std::max(st.vtx, relErr(a.position[k], b.position[k]));
        }
        st.uv = std::max({st.uv, std::fabs(static_cast<double>(a.texcoord[0]) - b.texcoord[0]),
                          std::fabs(static_cast<double>(a.texcoord[1]) - b.texcoord[1])});
        for (int ch = 0; ch < 4; ++ch) {
            const int ga = static_cast<int>((a.color >> (8 * ch)) & 0xFF), cb = static_cast<int>((b.color >> (8 * ch)) & 0xFF);
            st.colorOff += std::abs(ga - cb) > 1 ? 1u : 0u;
        }
    }
}

int runParity(std::uint32_t framesInFlight, bool oneStep) {
    if (!vulkanKernelsAvailable()) {
        std::printf("SKIP: particle kernels not built\n");
        return kSkip;
    }
    Context ctx;
    const int rc = setup(ctx, true);
    if (rc != 0) {
        return rc;
    }
    {
        constexpr std::uint32_t kFrames = 150;
        const ManagerConfig cfg = smallConfig(framesInFlight);
        ParticleSystemManager cpu(cfg), gpu(cfg);
        CpuParticleBackend cb(fuse::kernel::Backend::CpuReference);
        VulkanParticleBackend vb(*ctx.device, *ctx.allocator);
        CHECK(cb.init(cfg));
        if (!vb.init(cfg)) {
            std::fprintf(stderr, "FAIL: Vulkan backend init: %s\n", vb.lastError().c_str());
            return 1;
        }
        Driver dc(scenarios()), dg(scenarios());
        CHECK(dc.registerMeshes(cpu) && dg.registerMeshes(gpu));
        std::vector<Stats> per(dc.list.size());
        std::uint32_t counterMismatch = 0, snapshots = 0;
        std::vector<GpuParticle> gp;
        std::vector<GpuParticleVertex> gv;
        for (std::uint32_t f = 0; f < kFrames; ++f) {
            const bool snapshot = f % 10u == 9u || f + 1 == kFrames;
            if (oneStep && snapshot) {
                // One-step mode: the GPU starts this frame from the CPU reference's state.
                CHECK(vb.uploadParticles(cb.particles()));
            }
            CHECK(dc.run(cpu, cb, f, 1, [](std::uint32_t) {}));
            CHECK(dg.run(gpu, vb, f, 1, [](std::uint32_t) {}));
            for (std::uint32_t s = 0; s < cfg.maxSystems; ++s) {
                counterMismatch += cpu.counters(s) == gpu.counters(s) ? 0u : 1u;
            }
            if (snapshot) {
                if (!vb.readback(gp, gv)) {
                    std::fprintf(stderr, "FAIL: readback: %s\n", vb.lastError().c_str());
                    return 1;
                }
                for (std::size_t i = 0; i < dc.list.size(); ++i) {
                    const std::int32_t slot = cpu.findSystem(dc.hashes[i], dc.list[i].materialKey);
                    CHECK(slot >= 0 && slot == gpu.findSystem(dg.hashes[i], dg.list[i].materialKey));
                    if (slot < 0) {
                        continue;
                    }
                    const std::uint32_t max = dc.list[i].desc.gpu.maxNumParticles;
                    const std::uint32_t pb = cpu.particleBase(static_cast<std::uint32_t>(slot));
                    const std::uint32_t vbase = cpu.vertexBase(static_cast<std::uint32_t>(slot));
                    compareParticles(gp, cb.particles(), pb, pb + max, per[i]);
                    compareVertices(gv, cb.vertices(), vbase, vbase + max * verticesPerParticle(dc.list[i].desc), per[i]);
                }
                ++snapshots;
            }
        }
        std::printf("%s, frames in flight %u: %u snapshots, ring counter mismatches %u\n",
                    oneStep ? "one step from the CPU state" : "150-frame trajectories", framesInFlight, snapshots, counterMismatch);
        CHECK(counterMismatch == 0);
        for (std::size_t i = 0; i < dc.list.size(); ++i) {
            const Stats& st = per[i];
            // Turbulent systems are chaotic (see kTolerance): their trajectories are compared for integer state only;
            // their float state is held to kTolerance in the one-step mode.
            const bool chaotic = (dc.list[i].desc.gpu.flags & kFlagUseTurbulence) != 0u && !oneStep;
            const double tol = chaotic ? 1e30 : kTolerance;
            std::printf("  %-8s %6u live particles, %6u vertices; integer mismatches %u, colours off by > 1: %u; max relative error: "
                        "position %.2e velocity %.2e rotation %.2e vertex %.2e, uv %.2e (%s)\n",
                        dc.list[i].name.c_str(), st.compared, st.vertices, st.intMismatch, st.colorOff, st.pos, st.vel, st.rot, st.vtx,
                        st.uv, chaotic ? "chaotic: integer state only" : "tolerance 1e-4");
            CHECK(st.compared > 100 && st.vertices > 400);
            CHECK(st.intMismatch == 0 && (chaotic || st.colorOff == 0));
            CHECK(st.pos <= tol && st.vel <= tol && st.rot <= tol && st.vtx <= tol && st.uv <= tol);
        }
        CHECK(vb.waitIdle());
    }
    std::printf("validation messages: %u\n", g_messages);
    if (g_messages != 0u) {
        std::fprintf(stderr, "FAIL: %u validation message(s)\n", g_messages);
        return 1;
    }
    return 0;
}

int runZeroAlloc() {
    if (!vulkanKernelsAvailable()) {
        std::printf("SKIP: particle kernels not built\n");
        return kSkip;
    }
    Context ctx;
    const int rc = setup(ctx, false);
    if (rc != 0) {
        return rc;
    }
    const ManagerConfig cfg = smallConfig(2);
    ParticleSystemManager gpu(cfg);
    VulkanParticleBackend vb(*ctx.device, *ctx.allocator);
    if (!vb.init(cfg)) {
        std::fprintf(stderr, "FAIL: Vulkan backend init: %s\n", vb.lastError().c_str());
        return 1;
    }
    Driver d(scenarios());
    CHECK(d.registerMeshes(gpu));
    CHECK(d.run(gpu, vb, 0, 60, [](std::uint32_t) {}));
    g_allocs = 0;
    g_countAllocs = true;
    CHECK(d.run(gpu, vb, 60, 120, [](std::uint32_t) {}));
    g_countAllocs = false;
    CHECK(vb.waitIdle());
    std::printf("zero_alloc vulkan: %llu allocations over 120 steady frames\n", static_cast<unsigned long long>(g_allocs.load()));
    CHECK(g_allocs.load() == 0u);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string suite = argc > 1 ? argv[1] : "parity";
    std::uint32_t framesInFlight = 2;
    bool oneStep = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames-in-flight") == 0 && i + 1 < argc) {
            framesInFlight = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--one-step") == 0) {
            oneStep = true;
        }
    }
    int rc = 0;
    if (suite == "parity") {
        rc = runParity(framesInFlight, oneStep);
    } else if (suite == "zero_alloc") {
        rc = runZeroAlloc();
    } else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d check(s), rc %d\n", g_failures, rc);
        return 1;
    }
    std::printf("PASS rl_particles_vk_%s\n", suite.c_str());
    return 0;
}

#endif
