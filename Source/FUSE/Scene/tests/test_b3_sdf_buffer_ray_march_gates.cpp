// B3 gate row (master plan): "SceneData SDF object buffer uploads to GPU and ray marcher renders
// correct scene" — upload half on the Vulkan RHI, render half on the CPU reference ray marcher
// (the CUDA ray-march kernel is still a stub, so the row is only partially proven by this test).
//
// 1. An ECS scene (SceneManager) holds spheres, boxes (incl. a floor slab) and a capsule in view, a
//    hidden (visible = false) object in view, and objects behind the camera / outside the frustum.
// 2. SceneManager::buildFrame culls and builds SceneData; its `sdf_objects` are packed into the
//    ray marcher's GPU layout (`fuse::compute::SdfObject`, hard union) = the SDF object buffer.
// 3. The buffer is uploaded into a device-local storage buffer through the renderer's
//    ResourceManager (staging ring + upload queue) on Lavapipe and read back: byte-exact.
// 4. launch_ray_march_cpu renders the scene from the *read-back* buffer (depth + normal surfaces).
// 5. An independent reference — closed-form ray/sphere, ray/box (slab) and ray/capsule
//    intersections over the ECS components themselves (every visible SDF entity, culled or not) —
//    gives the exact hit mask, hit distance and surface normal per pixel. Pixels whose outcome
//    flips when every primitive is grown / shrunk by kAmbiguityEps (silhouettes, creases where
//    objects meet, box edges) are "ambiguous" and excluded; they must stay a small fraction.
//    Everywhere else: identical hit mask, |depth - ref| <= kDepthTol, dot(normal, ref) >= kNormalDot.
// 6. One entity moves; the frame is rebuilt, re-uploaded into the same GPU buffer, read back
//    byte-exact and re-rendered against the updated reference.
// The run is validation-clean when the Khronos validation layer is installed.
//
// Exit 77 (skip) without a Vulkan device (stub build / no ICD).
#include <fuse/compute/ray_march.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/scene/scene_manager.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::ecs::EntityID;
using fuse::math::Vec3;
using fuse::math::Vec4;

constexpr int kSkip = 77;
constexpr u32 kWidth = 160;
constexpr u32 kHeight = 120;
constexpr f32 kFovDeg = 60.f;
constexpr f32 kAmbiguityEps = 0.01f;
constexpr f32 kDepthTol = 2e-3f;
constexpr f32 kNormalDot = 0.999f;
constexpr f32 kMaxAmbiguousFraction = 0.06f;

int g_failures = 0;

void expectTrue(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------------------------
// SceneData -> SDF object buffer (ray marcher GPU layout)

u32 primitiveType(fuse::ecs::SDFPrimitive type) {
    switch (type) {
    case fuse::ecs::SDFPrimitive::Sphere:
        return static_cast<u32>(fuse::compute::SdfPrimitiveType::Sphere);
    case fuse::ecs::SDFPrimitive::Box:
        return static_cast<u32>(fuse::compute::SdfPrimitiveType::Box);
    case fuse::ecs::SDFPrimitive::Capsule:
        return static_cast<u32>(fuse::compute::SdfPrimitiveType::Capsule);
    case fuse::ecs::SDFPrimitive::Torus:
        return static_cast<u32>(fuse::compute::SdfPrimitiveType::Torus);
    default:
        return UINT32_MAX;
    }
}

/// The ray marcher's objects are axis-aligned and unscaled, combined by hard union here: packing
/// fails for anything else (so the gate can never silently render a different scene).
bool packSdfObjects(const fuse::ecs::SceneData& frame, std::vector<fuse::compute::SdfObject>& out) {
    out.assign(frame.sdf_objects.size(), fuse::compute::SdfObject{});
    // Deterministic bytes (the struct has no padding today; memset keeps the byte compare honest).
    std::memset(static_cast<void*>(out.data()), 0, out.size() * sizeof(fuse::compute::SdfObject));
    for (std::size_t i = 0; i < frame.sdf_objects.size(); ++i) {
        const fuse::ecs::SceneSdfObject& item = frame.sdf_objects[i];
        const f32* m = item.transform.data.data();
        const bool axisAligned = std::abs(m[0] - 1.f) < 1e-6f && std::abs(m[5] - 1.f) < 1e-6f &&
                                 std::abs(m[10] - 1.f) < 1e-6f && std::abs(m[1]) < 1e-6f && std::abs(m[2]) < 1e-6f &&
                                 std::abs(m[4]) < 1e-6f && std::abs(m[6]) < 1e-6f && std::abs(m[8]) < 1e-6f &&
                                 std::abs(m[9]) < 1e-6f;
        const u32 type = primitiveType(item.type);
        if (!axisAligned || type == UINT32_MAX || item.op != fuse::ecs::SDFCsgOp::Union || item.roughness != 0.f) {
            return false;
        }
        fuse::compute::SdfObject& obj = out[i];
        obj.position = Vec3{m[12], m[13], m[14]};
        obj.params = Vec3{item.params.x, item.params.y, item.params.z};
        obj.type = type;
        obj.alpha = 0.f; // hard union
        obj.material_id = item.material_id;
        obj.rounding = 0.f;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Independent analytic reference: closed-form ray intersections against the ECS components.

struct RefPrimitive {
    fuse::ecs::SDFPrimitive type;
    Vec3 center;
    Vec3 params;
    u32 id; ///< entity index
};

struct RefHit {
    bool hit = false;
    f32 t = 0.f;
    Vec3 normal{};
    u32 id = UINT32_MAX;
};

f32 dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// First entry of the ray into a sphere (camera outside every solid).
bool raySphere(const Vec3& o, const Vec3& d, const Vec3& c, f32 r, f32& t, Vec3& n) {
    const Vec3 oc = o - c;
    const f32 b = dot(oc, d);
    const f32 cc = dot(oc, oc) - r * r;
    const f32 disc = b * b - cc;
    if (disc < 0.f) {
        return false;
    }
    const f32 root = -b - std::sqrt(disc);
    if (root <= 0.f) {
        return false;
    }
    t = root;
    n = ((o + d * root) - c) * (1.f / r);
    return true;
}

bool rayBox(const Vec3& o, const Vec3& d, const Vec3& c, const Vec3& h, f32& t, Vec3& n) {
    const f32 oc[3] = {o.x - c.x, o.y - c.y, o.z - c.z};
    const f32 dir[3] = {d.x, d.y, d.z};
    const f32 half[3] = {h.x, h.y, h.z};
    f32 tNear = -1e30f;
    f32 tFar = 1e30f;
    int axis = -1;
    f32 sign = 0.f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-12f) {
            if (std::abs(oc[i]) > half[i]) {
                return false;
            }
            continue;
        }
        f32 t0 = (-half[i] - oc[i]) / dir[i];
        f32 t1 = (half[i] - oc[i]) / dir[i];
        f32 s = -1.f; // entering through the -half face
        if (t0 > t1) {
            std::swap(t0, t1);
            s = 1.f;
        }
        if (t0 > tNear) {
            tNear = t0;
            axis = i;
            sign = s;
        }
        tFar = std::min(tFar, t1);
    }
    if (axis < 0 || tNear > tFar || tNear <= 0.f) {
        return false;
    }
    t = tNear;
    n = Vec3{axis == 0 ? sign : 0.f, axis == 1 ? sign : 0.f, axis == 2 ? sign : 0.f};
    return true;
}

/// Y-axis capsule: union of the finite cylinder side (|y| <= h) and the two end spheres.
bool rayCapsule(const Vec3& o, const Vec3& d, const Vec3& c, f32 r, f32 h, f32& t, Vec3& n) {
    bool found = false;
    const Vec3 local = o - c;
    const f32 a = d.x * d.x + d.z * d.z;
    if (a > 1e-12f) {
        const f32 b = local.x * d.x + local.z * d.z;
        const f32 cc = local.x * local.x + local.z * local.z - r * r;
        const f32 disc = b * b - a * cc;
        if (disc >= 0.f) {
            const f32 root = (-b - std::sqrt(disc)) / a;
            const f32 y = local.y + d.y * root;
            if (root > 0.f && std::abs(y) <= h) {
                t = root;
                const Vec3 p = local + d * root;
                n = Vec3{p.x / r, 0.f, p.z / r};
                found = true;
            }
        }
    }
    for (f32 capY : {h, -h}) {
        f32 tc = 0.f;
        Vec3 nc{};
        if (raySphere(o, d, c + Vec3{0.f, capY, 0.f}, r, tc, nc) && (!found || tc < t)) {
            t = tc;
            n = nc;
            found = true;
        }
    }
    return found;
}

/// Nearest hit over all primitives, each grown by `grow` (shrunk when negative).
RefHit traceReference(const std::vector<RefPrimitive>& prims, const Vec3& o, const Vec3& d, f32 grow) {
    RefHit best{};
    for (const RefPrimitive& p : prims) {
        f32 t = 0.f;
        Vec3 n{};
        bool hit = false;
        switch (p.type) {
        case fuse::ecs::SDFPrimitive::Sphere:
            hit = raySphere(o, d, p.center, p.params.x + grow, t, n);
            break;
        case fuse::ecs::SDFPrimitive::Box:
            hit = rayBox(o, d, p.center, Vec3{p.params.x + grow, p.params.y + grow, p.params.z + grow}, t, n);
            break;
        case fuse::ecs::SDFPrimitive::Capsule:
            hit = rayCapsule(o, d, p.center, p.params.x + grow, std::max(p.params.y, 0.f), t, n);
            break;
        default:
            break;
        }
        if (hit && (!best.hit || t < best.t)) {
            best.hit = true;
            best.t = t;
            best.normal = n;
            best.id = p.id;
        }
    }
    return best;
}

/// Every visible SDF entity straight from the ECS (not from SceneData: culling must not lose any).
std::vector<RefPrimitive> referencePrimitives(fuse::ecs::Registry& reg) {
    std::vector<RefPrimitive> prims;
    reg.each<fuse::ecs::SDFObject, fuse::ecs::Transform>(
        [&](EntityID id, fuse::ecs::SDFObject& sdf, fuse::ecs::Transform& transform) {
            if (!sdf.visible) {
                return;
            }
            prims.push_back({sdf.type, Vec3{transform.position.x, transform.position.y, transform.position.z},
                             Vec3{sdf.params.x, sdf.params.y, sdf.params.z}, id.index});
        });
    return prims;
}

struct Camera {
    Vec3 position;
    Vec3 forward;
    Vec3 right;
    Vec3 up;
    f32 fovRad;
};

/// Pixel-centre ray, row 0 = top (the camera model documented on RayMarchParams).
Vec3 pixelRay(const Camera& cam, u32 x, u32 y) {
    const f32 tanHalf = std::tan(0.5f * cam.fovRad);
    const f32 aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);
    const f32 ndcX = 2.f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth) - 1.f;
    const f32 ndcY = 1.f - 2.f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kHeight);
    return (cam.forward + cam.right * (ndcX * tanHalf * aspect) + cam.up * (ndcY * tanHalf)).normalized();
}

struct Comparison {
    u32 hits = 0;
    u32 ambiguous = 0;
    u32 maskMismatch = 0;
    u32 depthMismatch = 0;
    u32 normalMismatch = 0;
    f32 maxDepthError = 0.f;
    f32 minNormalDot = 1.f;
    std::set<u32> objectsSeen;
};

Comparison compareFrame(const std::vector<RefPrimitive>& prims, const Camera& cam, const std::vector<f32>& depth,
                        const std::vector<Vec4>& normals) {
    Comparison c{};
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const Vec3 d = pixelRay(cam, x, y);
            const RefHit ref = traceReference(prims, cam.position, d, 0.f);
            const RefHit grown = traceReference(prims, cam.position, d, kAmbiguityEps);
            const RefHit shrunk = traceReference(prims, cam.position, d, -kAmbiguityEps);
            const bool ambiguous = ref.hit != grown.hit || ref.hit != shrunk.hit ||
                                   (ref.hit && (ref.id != grown.id || ref.id != shrunk.id ||
                                                dot(ref.normal, grown.normal) < 0.9999f ||
                                                dot(ref.normal, shrunk.normal) < 0.9999f));
            if (ambiguous) {
                ++c.ambiguous;
                continue;
            }
            const u32 i = y * kWidth + x;
            const bool cpuHit = depth[i] >= 0.f;
            if (cpuHit != ref.hit) {
                ++c.maskMismatch;
                continue;
            }
            if (!ref.hit) {
                continue;
            }
            ++c.hits;
            c.objectsSeen.insert(ref.id);
            const f32 depthError = std::abs(depth[i] - ref.t);
            c.maxDepthError = std::max(c.maxDepthError, depthError);
            c.depthMismatch += depthError > kDepthTol ? 1u : 0u;
            const f32 nd = dot(Vec3{normals[i].x, normals[i].y, normals[i].z}, ref.normal);
            c.minNormalDot = std::min(c.minNormalDot, nd);
            c.normalMismatch += (nd < kNormalDot || normals[i].w != 1.f) ? 1u : 0u;
        }
    }
    return c;
}

struct Body {
    const char* name;
    fuse::ecs::SDFPrimitive type;
    Vec3 position;
    Vec3 params;
    bool visible;
    bool inView; ///< expected in SceneData (in the frustum and visible)
};

void setTransform(fuse::ecs::Registry& reg, EntityID id, const Vec3& p) {
    fuse::ecs::Transform* t = reg.get<fuse::ecs::Transform>(id);
    t->position = {p.x, p.y, p.z, 1.f};
    t->dirty = true;
}

/// Build, pack, upload, read back, render and compare one frame. Returns false on a hard failure.
bool runFrame(const char* label, fuse::scene::SceneManager& scene, const Camera& cam,
              fuse::renderer::ResourceManager& resources, fuse::renderer::BufferHandle& gpuBuffer,
              const std::vector<EntityID>& expectedInFrame, std::vector<f32>& depthOut) {
    scene.update(0.f);
    fuse::ecs::CullResult cull{};
    const fuse::ecs::SceneData frame = scene.buildFrame(&cull);
    std::printf("[%s] SceneData: %zu SDF objects (culled %u)\n", label, frame.sdf_objects.size(), cull.culled_count);

    std::set<u32> inFrame;
    for (const fuse::ecs::SceneSdfObject& item : frame.sdf_objects) {
        inFrame.insert(item.entity.index);
    }
    std::set<u32> expected;
    for (EntityID id : expectedInFrame) {
        expected.insert(id.index);
    }
    expectTrue(inFrame == expected, std::string(label) + ": SceneData holds exactly the visible in-frustum SDF objects");

    std::vector<fuse::compute::SdfObject> packed;
    if (!packSdfObjects(frame, packed)) {
        expectTrue(false, std::string(label) + ": SceneData packs into the ray marcher layout");
        return false;
    }
    const std::size_t bytes = packed.size() * sizeof(fuse::compute::SdfObject);

    // --- upload through the renderer, read back, byte-exact ---
    if (!gpuBuffer.isValid()) {
        fuse::renderer::BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<fuse::renderer::BufferUsage>(
            static_cast<u32>(fuse::renderer::BufferUsage::Storage) |
            static_cast<u32>(fuse::renderer::BufferUsage::TransferSrc) |
            static_cast<u32>(fuse::renderer::BufferUsage::TransferDst));
        desc.memoryUsage = fuse::renderer::MemoryUsage::GpuOnly;
        desc.name = "SceneData SDF objects";
        gpuBuffer = resources.createBuffer(desc);
        expectTrue(gpuBuffer.isValid(), std::string(label) + ": device-local SDF object buffer created");
        if (!gpuBuffer.isValid()) {
            return false;
        }
    }
    const fuse::renderer::UploadTicket ticket = resources.uploadBuffer(gpuBuffer, packed.data(), bytes);
    expectTrue(ticket.isValid(), std::string(label) + ": upload accepted");
    expectTrue(resources.waitUpload(ticket), std::string(label) + ": upload completed (fence)");
    std::vector<fuse::compute::SdfObject> readback(packed.size());
    std::memset(static_cast<void*>(readback.data()), 0xCD, bytes);
    expectTrue(resources.readBuffer(gpuBuffer, readback.data(), bytes), std::string(label) + ": GPU buffer read back");
    const bool byteExact = std::memcmp(readback.data(), packed.data(), bytes) == 0;
    std::printf("[%s] uploaded %zu bytes (%zu objects x %zu B), serial %llu, read back %s\n", label, bytes,
                packed.size(), sizeof(fuse::compute::SdfObject), static_cast<unsigned long long>(ticket.serial),
                byteExact ? "byte-exact" : "MISMATCH");
    expectTrue(byteExact, std::string(label) + ": read-back SDF object buffer is byte-exact");

    // --- render from the read-back buffer on the CPU reference ray marcher ---
    std::vector<f32> depth(kWidth * kHeight, -2.f);
    std::vector<Vec4> normals(kWidth * kHeight);
    fuse::compute::RayMarchParams params{};
    params.depth_surface = depth.data();
    params.output_surface = normals.data();
    params.width = kWidth;
    params.height = kHeight;
    params.cam_pos = cam.position;
    params.cam_forward = cam.forward;
    params.cam_right = cam.right;
    params.cam_up = cam.up;
    params.fov_rad = cam.fovRad;
    params.max_steps = 512;
    params.min_dist = 1e-4f;
    params.max_dist = 100.f;
    params.objects = readback.data();
    params.object_count = static_cast<u32>(readback.size());
    expectTrue(fuse::compute::launch_ray_march_cpu(params), std::string(label) + ": CPU ray march ran");

    // --- compare against the analytic reference of the ECS scene ---
    const std::vector<RefPrimitive> prims = referencePrimitives(scene.registry());
    const Comparison c = compareFrame(prims, cam, depth, normals);
    const u32 pixels = kWidth * kHeight;
    std::printf("[%s] %ux%u: %u hit px, %u ambiguous px (%.2f%%), mask mismatches %u, depth mismatches %u "
                "(max err %.2e), normal mismatches %u (min dot %.6f), objects seen %zu\n",
                label, kWidth, kHeight, c.hits, c.ambiguous, 100.0 * c.ambiguous / pixels, c.maskMismatch,
                c.depthMismatch, static_cast<double>(c.maxDepthError), c.normalMismatch,
                static_cast<double>(c.minNormalDot), c.objectsSeen.size());
    expectTrue(c.maskMismatch == 0, std::string(label) + ": hit mask matches the analytic scene");
    expectTrue(c.depthMismatch == 0, std::string(label) + ": hit depths match the analytic intersections");
    expectTrue(c.normalMismatch == 0, std::string(label) + ": normals match the analytic surface normals");
    expectTrue(static_cast<f32>(c.ambiguous) <= kMaxAmbiguousFraction * static_cast<f32>(pixels),
               std::string(label) + ": ambiguous (edge) pixels stay a small fraction");
    expectTrue(c.hits > pixels / 8, std::string(label) + ": the scene covers a good part of the frame");
    std::set<u32> expectedSeen = expected;
    expectTrue(c.objectsSeen == expectedSeen, std::string(label) + ": every in-frame object is visible in the render");
    depthOut = depth;
    return true;
}

} // namespace

int main() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = true;
    bootstrapDesc.createSwapchain = false;
    bootstrapDesc.createFrameManager = false;
    fuse::renderer::resetVulkanValidationCounters();
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    if (bootstrap == nullptr || !bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — the SDF buffer upload needs an ICD (Lavapipe in CI)\n");
        return kSkip;
    }
    std::printf("device: %s\n", bootstrap->device()->info().deviceName.c_str());

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());
    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

    // --- ECS scene ---
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc sceneDesc{};
    sceneDesc.hasVoxels = false;
    scene.init(sceneDesc);
    auto& reg = scene.registry();
    using P = fuse::ecs::SDFPrimitive;
    const Body bodies[] = {
        {"sphere A", P::Sphere, {-2.f, 0.5f, 0.f}, {1.f, 0.f, 0.f}, true, true},
        {"sphere B", P::Sphere, {0.2f, 1.4f, 1.5f}, {0.75f, 0.f, 0.f}, true, true},
        {"box", P::Box, {2.2f, 0.2f, 0.5f}, {1.f, 0.6f, 0.8f}, true, true},
        {"floor slab", P::Box, {0.f, -1.25f, 2.f}, {6.f, 0.25f, 6.f}, true, true},
        {"sphere in floor", P::Sphere, {-0.5f, -0.7f, -1.5f}, {0.6f, 0.f, 0.f}, true, true},
        {"capsule", P::Capsule, {1.f, 0.3f, -2.f}, {0.35f, 0.7f, 0.f}, true, true},
        {"hidden sphere", P::Sphere, {0.f, 0.3f, -1.f}, {0.5f, 0.f, 0.f}, false, false},
        {"behind camera", P::Sphere, {0.f, 1.f, -20.f}, {0.5f, 0.f, 0.f}, true, false},
        {"off to the side", P::Box, {60.f, 0.f, 0.f}, {0.5f, 0.5f, 0.5f}, true, false},
    };
    std::vector<EntityID> ids;
    std::vector<EntityID> expectedInFrame;
    u32 material = 1;
    for (const Body& body : bodies) {
        const EntityID id = reg.create();
        fuse::ecs::Transform t{};
        t.position = {body.position.x, body.position.y, body.position.z, 1.f};
        t.dirty = true;
        reg.add(id, t);
        fuse::ecs::SDFObject sdf{};
        sdf.type = body.type;
        sdf.op = fuse::ecs::SDFCsgOp::Union;
        sdf.params = {body.params.x, body.params.y, body.params.z, 0.f};
        sdf.material_id = material++;
        sdf.visible = body.visible;
        reg.add(id, sdf);
        ids.push_back(id);
        if (body.inView) {
            expectedInFrame.push_back(id);
        }
    }
    const EntityID cameraId = scene.createCamera(kFovDeg, true);
    fuse::ecs::Camera* ecsCamera = reg.get<fuse::ecs::Camera>(cameraId);
    ecsCamera->aspect_ratio = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);
    ecsCamera->near_plane = 0.1f;
    ecsCamera->far_plane = 100.f;
    setTransform(reg, cameraId, {0.f, 1.f, -10.f});
    scene.update(0.f);

    // Ray-march camera = the ECS camera's world frame (columns of local_to_world; forward = +Z).
    const fuse::ecs::Transform* camTransform = reg.get<fuse::ecs::Transform>(cameraId);
    const f32* m = camTransform->local_to_world.data.data();
    const Camera cam{Vec3{m[12], m[13], m[14]}, Vec3{m[8], m[9], m[10]}.normalized(), Vec3{m[0], m[1], m[2]}.normalized(),
                     Vec3{m[4], m[5], m[6]}.normalized(), kFovDeg * 3.14159265358979f / 180.f};

    fuse::renderer::BufferHandle gpuBuffer{};
    std::vector<f32> depthA;
    std::vector<f32> depthB;
    runFrame("frame 0", scene, cam, resources, gpuBuffer, expectedInFrame, depthA);

    // Move sphere A across the frame; rebuild + re-upload into the same GPU buffer.
    setTransform(reg, ids[0], {-1.2f, 2.4f, 3.f});
    runFrame("frame 1 (sphere A moved)", scene, cam, resources, gpuBuffer, expectedInFrame, depthB);
    u32 changed = 0;
    for (std::size_t i = 0; i < depthA.size() && i < depthB.size(); ++i) {
        changed += std::abs(depthA[i] - depthB[i]) > 1e-3f ? 1u : 0u;
    }
    std::printf("frame 1 vs frame 0: %u px changed\n", changed);
    expectTrue(changed > 200u, "moving an entity changes the rendered frame");

    if (gpuBuffer.isValid()) {
        resources.destroyBuffer(gpuBuffer);
    }
    resources.destroy();
    bindless.destroy(*bootstrap->device());
    scene.destroy();
    bootstrap.reset();
    const fuse::renderer::VulkanValidationCounters counters = fuse::renderer::vulkanValidationCounters();
    std::printf("validation: errors=%u warnings=%u%s%s\n", counters.errors, counters.warnings,
                counters.lastError.empty() ? "" : " last: ", counters.lastError.c_str());
    expectTrue(counters.errors == 0u, "zero validation errors");

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b3_sdf_buffer_ray_march: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_b3_sdf_buffer_ray_march: all checks passed (CPU reference ray marcher; CUDA kernel is a stub)\n");
    return EXIT_SUCCESS;
}
