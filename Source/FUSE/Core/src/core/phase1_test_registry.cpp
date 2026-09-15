#include <fuse/core/phase1_test_registry.hpp>

#include <fuse/core/init.hpp>
#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/math/aabb.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/math/quat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/platform/window.hpp>
#include <fuse/profiler/profiler.hpp>

#include <atomic>
#include <cmath>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fuse::core {

namespace {

const std::vector<Phase1Deliverable> kChecklist = {
    {"build.umbrella_cmake", "Umbrella CMake builds fuse_core on Linux CI", Phase1Module::Build, true, true},
    {"build.cpp23_host", "C++23 host dialect with zero-warning CI gate", Phase1Module::Build, false, false},
    {"build.shipping_preset", "Shipping preset strips assert/profiler macros", Phase1Module::Build, false, false},
    {"types.fixed_width_aliases", "fuse::u8/u32/f32 aliases on public API", Phase1Module::Types, true, true},
    {"types.generation_handles", "Handle<T> generation/epoch invalidation", Phase1Module::Types, true, true},
    {"types.object_hierarchy", "fuse::Object handle publish path", Phase1Module::Types, true, true},
    {"memory.frame_allocator", "Per-frame bump allocator reset semantics", Phase1Module::Memory, true, true},
    {"memory.pool_allocator", "Pool allocator alloc/free and generation invalidation", Phase1Module::Memory, true, true},
    {"memory.gpu_allocator", "CUDA device/pinned/managed allocators", Phase1Module::Memory, false, false},
    {"math.mat4_transform", "Mat4 TRS + transformPoint match reference", Phase1Module::Math, true, true},
    {"math.quat_slerp", "Quaternion slerp stays unit length", Phase1Module::Math, true, true},
    {"math.sdf_primitives", "SDF primitives match ray march reference", Phase1Module::Math, false, false},
    {"jobs.scheduler_init", "JobScheduler initialises worker threads", Phase1Module::Jobs, true, true},
    {"jobs.dependency_chain", "JobCounter dependency ordering", Phase1Module::Jobs, true, true},
    {"jobs.parallel_for", "parallel_for matches serial loop", Phase1Module::Jobs, true, true},
    {"jobs.cuda_lane", "CUDA job lane dispatches when toolkit present", Phase1Module::Jobs, true, true},
    {"logging.async_ring", "Lock-free async logger ring buffer", Phase1Module::Logging, false, false},
    {"logging.profiler_scopes", "CPU ProfileScope ring buffer + chrome JSON", Phase1Module::Logging, true, true},
    {"logging.assert_macros", "FUSE_ASSERT/FUSE_VERIFY fatal hook path", Phase1Module::Logging, true, true},
    {"platform.window_stub", "Window stub stores metadata and null native handle", Phase1Module::Platform, true, true},
    {"platform.event_pump", "EventPump synthetic queue and quit flow", Phase1Module::Platform, true, true},
    {"platform.vulkan_surface", "get_vulkan_surface returns valid VkSurfaceKHR", Phase1Module::Platform, false, false},
    {"io.vfs_async_load", "VFS async read publishes through HandleTable", Phase1Module::Types, true, true},
};

bool smokeJobsAndMath() {
    std::atomic<u32> completed{0};
    std::vector<f32> transformed(64, 0.f);

    const fuse::math::Vec3 position{1.f, 2.f, 3.f};
    const fuse::math::Quat rotation = fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 0.25f);
    const fuse::math::Vec3 scale{2.f, 2.f, 2.f};
    const fuse::math::Mat4 matrix = fuse::math::fromTRS(position, rotation, scale);
    const fuse::math::Vec3 probe{0.5f, 0.f, 0.f};
    const fuse::math::Vec3 expected = fuse::math::transformPoint(matrix, probe);

    fuse::jobs::parallel_for(0u, static_cast<u32>(transformed.size()), 8u, [&](u32 index) {
        const fuse::math::Vec3 point{
            probe.x + static_cast<f32>(index) * 0.001f,
            probe.y,
            probe.z,
        };
        const fuse::math::Vec3 result = fuse::math::transformPoint(matrix, point);
        transformed[index] = result.x + result.y + result.z;
        completed.fetch_add(1u, std::memory_order_relaxed);
    });

    if (completed.load(std::memory_order_acquire) != transformed.size()) {
        return false;
    }

    const f32 reference = expected.x + expected.y + expected.z;
    if (std::fabs(transformed[0] - reference) > 0.01f) {
        return false;
    }

    const fuse::math::AABB bounds{{0.f, 0.f, 0.f}, {4.f, 4.f, 4.f}};
    return bounds.contains(expected);
}

bool smokeHandlesAndVfsAsync() {
    const std::string tempPath = "/tmp/fuse_b18_phase1_asset.bin";
    {
        std::ofstream out(tempPath, std::ios::binary);
        if (!out) {
            return false;
        }
        out << "fuse-b18";
    }

    auto& vfs = fuse::io::VirtualFileSystem::instance();
    vfs.mount(fuse::io::MountKind::Game, "/tmp", "/game");

    fuse::HandleTable<fuse::io::Asset> table;
    const fuse::io::LoadId loadId = vfs.submitLoadAsync("/game/fuse_b18_phase1_asset.bin");
    if (loadId == 0u) {
        return false;
    }

    u32 spinGuard = 0u;
    while (vfs.completedLoadCount() == 0u && spinGuard < 1'000'000u) {
        std::this_thread::yield();
        ++spinGuard;
    }
    if (vfs.completedLoadCount() == 0u) {
        return false;
    }

    if (vfs.drainCompletedLoads(table) != 1u) {
        return false;
    }

    std::vector<fuse::Handle<fuse::io::Asset>> committed;
    if (table.commit(&committed) != 1u) {
        return false;
    }
    if (!table.valid(committed[0])) {
        return false;
    }

    const fuse::io::Asset* live = table.get(committed[0]);
    return live != nullptr && live->bytes.size() == 8u &&
           live->virtualPath == "/game/fuse_b18_phase1_asset.bin";
}

bool smokeProfilerScopes() {
    fuse::profiler::setEnabled(true);
    fuse::profiler::reset();
    fuse::profiler::beginFrame();

    {
        FUSE_PROFILE_SCOPE("phase1_jobs_math");
        if (!smokeJobsAndMath()) {
            fuse::profiler::endFrame();
            return false;
        }
    }

    {
        FUSE_PROFILE_SCOPE("phase1_vfs_handles");
        if (!smokeHandlesAndVfsAsync()) {
            fuse::profiler::endFrame();
            return false;
        }
    }

    fuse::profiler::endFrame();

    if (fuse::profiler::eventCount() < 4u) {
        return false;
    }

    const std::string trace = fuse::profiler::exportChromeTraceJson();
    return trace.find("\"traceEvents\"") != std::string::npos &&
           trace.find("phase1_jobs_math") != std::string::npos;
}

bool smokePlatformWindow() {
    fuse::platform::WindowDesc desc{};
    desc.title = "FUSE Phase1 Smoke";
    desc.width = 640;
    desc.height = 480;
    fuse::platform::Window window(desc);
    const fuse::platform::VulkanSurfaceWire wire = window.vulkanSurfaceWire();
    return window.isValid() && window.width() == 640u && !wire.presentable &&
           wire.nativeSurface == nullptr;
}

} // namespace

const std::vector<Phase1Deliverable>& Phase1TestRegistry::checklist() {
    return kChecklist;
}

u32 Phase1TestRegistry::countByModule(Phase1Module module) {
    u32 count = 0;
    for (const Phase1Deliverable& item : kChecklist) {
        if (item.module == module) {
            ++count;
        }
    }
    return count;
}

u32 Phase1TestRegistry::stubLandedCount() {
    u32 count = 0;
    for (const Phase1Deliverable& item : kChecklist) {
        if (item.stub_landed) {
            ++count;
        }
    }
    return count;
}

u32 Phase1TestRegistry::automatedCount() {
    u32 count = 0;
    for (const Phase1Deliverable& item : kChecklist) {
        if (item.automated) {
            ++count;
        }
    }
    return count;
}

bool Phase1TestRegistry::runIntegrationSmoke() {
    initialize();

    const bool ok = smokeProfilerScopes() && smokePlatformWindow();

    shutdown();
    return ok;
}

} // namespace fuse::core
