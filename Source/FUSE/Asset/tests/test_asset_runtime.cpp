// UNI-U7-ASSET-1 CPU gates for the runtime asset core (fuse_asset).
//
//   id        AssetId normalisation / FNV-1a vectors / text form; ecs::MeshAssets on an entity
//   formats   runtime readers on hand-built FMSH v1, .fusetex and .fusemat blobs (+ corruption)
//   registry  AssetRegistry: VFS read -> job decode -> render upload -> commit; refcount, unload,
//             reload, failures, keepCpuData=false
//   cancel    release() while queued / decoded (upload queued) / uploaded (publish pending); re-acquire
//             during a stale load; destroying the registry with loads in flight
//   mpsc      RenderUploadQueue with 4 producers + 1 consumer (per-producer FIFO, no loss), then the
//             whole pipeline with JobScheduler workers, a render thread and a churning game thread;
//             every GPU resource uploaded is released (run under TSan by the nightly job)
//   material  runtime .fusemat reader == the renderer's read_fusemat_binary (skips without renderer)
//   cook      glTF cube + PNG cooked with fuse_cook (tool when given, else the cook library), plus an
//             FMSH v2 mesh with every W0.2 section; loaded through AssetRegistry; decoded data equals
//             the cook library's reader output (skips without Tools/FUSE/Cook)

#include <fuse/asset/asset_id.hpp>
#include <fuse/asset/asset_registry.hpp>
#include <fuse/asset/cooked_material.hpp>
#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/asset/cooked_texture.hpp>
#include <fuse/asset/render_upload.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#if defined(FUSE_ASSET_TEST_HAS_COOK)
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/cook/texture_cook.hpp>
#endif
#if defined(FUSE_ASSET_TEST_HAS_FUSEMAT)
#include <fuse/renderer/material_layers/fusemat.hpp>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace fuse;
using namespace fuse::asset;

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

fs::path scratch_dir(const char* suite) {
    const fs::path dir = fs::temp_directory_path() /
                         ("fuse_asset_runtime_" + std::string(suite) + "_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, const std::vector<u8>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

template <typename Pred>
bool wait_until(Pred pred, u32 timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

// ---- hand-built cooked blobs (no cook library needed) ---------------------------------------------

void put_u32(std::vector<u8>& out, u32 v) {
    for (u32 s = 0; s < 32u; s += 8u) {
        out.push_back(static_cast<u8>((v >> s) & 0xFFu));
    }
}
void put_f32(std::vector<u8>& out, f32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, 4u);
    put_u32(out, bits);
}
u64 fnv(const u8* d, usize n) {
    u64 h = 14695981039346656037ull;
    for (usize i = 0; i < n; ++i) {
        h ^= d[i];
        h *= 1099511628211ull;
    }
    return h;
}
void put_u64(std::vector<u8>& out, u64 v) {
    put_u32(out, static_cast<u32>(v));
    put_u32(out, static_cast<u32>(v >> 32));
}

/// A one-submesh FMSH v1 quad (4 vertices, 2 triangles) scaled by `scale` so every file differs.
CookedMesh make_quad(f32 scale) {
    CookedMesh m;
    m.positions = {0.f, 0.f, 0.f, scale, 0.f, 0.f, scale, scale, 0.f, 0.f, scale, 0.f};
    m.normals = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    m.uvs = {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    m.indices = {0u, 1u, 2u, 0u, 2u, 3u};
    m.submeshes.push_back(CookedMesh::Submesh{0u, 6u, 0u, 0u});
    m.bounds_max[0] = scale;
    m.bounds_max[1] = scale;
    return m;
}

/// FMSH v1 layout (see fuse::cook::serialize_cooked_mesh): header, submeshes, pos/nrm/uv, indices, FNV.
std::vector<u8> fmsh_v1(const CookedMesh& m) {
    std::vector<u8> out = {'F', 'M', 'S', 'H'};
    put_u32(out, 1u);
    put_u32(out, 0u);
    put_u32(out, m.vertex_count());
    put_u32(out, static_cast<u32>(m.indices.size()));
    put_u32(out, static_cast<u32>(m.submeshes.size()));
    for (f32 v : m.bounds_min) {
        put_f32(out, v);
    }
    for (f32 v : m.bounds_max) {
        put_f32(out, v);
    }
    for (const CookedMesh::Submesh& s : m.submeshes) {
        put_u32(out, s.index_offset);
        put_u32(out, s.index_count);
        put_u32(out, s.vertex_offset);
        put_u32(out, s.material_index);
    }
    for (f32 v : m.positions) {
        put_f32(out, v);
    }
    for (f32 v : m.normals) {
        put_f32(out, v);
    }
    for (f32 v : m.uvs) {
        put_f32(out, v);
    }
    for (u32 i : m.indices) {
        put_u32(out, i);
    }
    put_u64(out, fnv(out.data(), out.size()));
    return out;
}

/// An 8x8 BC7 .fusetex with 2 mips (4 + 1 blocks), block bytes filled with `seed`.
std::vector<u8> fusetex_bc7(u8 seed) {
    const std::string header = "FUSETEX_BC7\nformat_version=2\nhook=test\ncompression=BC7\nwidth=8\nheight=8\n"
                               "layers=1\ncube=0\nsrgb=1\nblock_bytes=16\nblocks=5\nmipmaps=on\nmip_levels=2\nDATA\n";
    std::vector<u8> out(header.begin(), header.end());
    for (u32 i = 0; i < 5u * 16u; ++i) {
        out.push_back(static_cast<u8>(seed + i));
    }
    return out;
}

void put_str(std::vector<u8>& out, const std::string& s) {
    out.push_back(static_cast<u8>(s.size() & 0xFFu));
    out.push_back(static_cast<u8>(s.size() >> 8));
    out.insert(out.end(), s.begin(), s.end());
}

/// .fusemat binary (see fuse/asset/cooked_material.hpp) of `m`.
std::vector<u8> fusemat_bytes(const CookedMaterial& m) {
    std::vector<u8> p;
    put_str(p, m.name);
    put_u32(p, m.shading);
    put_u32(p, m.category);
    put_u32(p, m.wind);
    for (f32 a : m.albedo) {
        put_f32(p, a);
    }
    put_f32(p, m.roughness);
    put_f32(p, m.metallic);
    put_f32(p, m.normal_strength);
    put_str(p, m.textures.albedo);
    put_str(p, m.textures.normal);
    put_f32(p, m.uv_scale);
    put_u32(p, m.triplanar ? 1u : 0u);
    put_f32(p, m.triplanar_sharpness);
    put_u32(p, m.stochastic ? 1u : 0u);
    put_f32(p, m.stochastic_lattice);
    put_f32(p, m.macro_scale);
    put_f32(p, m.macro_strength);
    put_str(p, m.detail.albedo);
    put_str(p, m.detail.normal);
    put_f32(p, m.detail_scale);
    put_f32(p, m.detail_strength);
    put_f32(p, m.detail_fade[0]);
    put_f32(p, m.detail_fade[1]);
    put_u32(p, static_cast<u32>(m.layers.size()));
    for (const CookedMaterialLayer& l : m.layers) {
        put_str(p, l.name);
        put_u32(p, l.mode);
        put_u32(p, l.mask);
        put_f32(p, l.mask_bias);
        put_f32(p, l.mask_scale);
        put_f32(p, l.coverage);
        put_f32(p, l.contrast);
        for (f32 a : l.albedo) {
            put_f32(p, a);
        }
        put_f32(p, l.roughness);
        put_f32(p, l.metallic);
        put_f32(p, l.uv_scale);
        put_f32(p, l.normal_strength);
        put_str(p, l.textures.albedo);
        put_str(p, l.textures.normal);
    }
    put_u32(p, m.procedural_function);
    put_u32(p, static_cast<u32>(m.procedural_params.size()));
    for (f32 v : m.procedural_params) {
        put_f32(p, v);
    }
    std::vector<u8> out;
    put_u32(out, kCookedMaterialMagic);
    put_u32(out, m.version);
    put_u32(out, static_cast<u32>(p.size()));
    out.insert(out.end(), p.begin(), p.end());
    put_u64(out, fnv(out.data(), out.size()));
    return out;
}

CookedMaterial sample_material() {
    CookedMaterial m;
    m.name = "rock/test";
    m.shading = 0u;
    m.category = 1u;
    m.albedo[0] = 0.4f;
    m.roughness = 0.7f;
    m.textures.albedo = "rock_albedo";
    m.textures.normal = "rock_normal";
    CookedMaterialLayer moss;
    moss.name = "moss";
    moss.mask = 5u;
    moss.albedo[0] = 0.2f;
    moss.albedo[1] = 0.4f;
    moss.albedo[2] = 0.1f;
    moss.coverage = 0.5f;
    m.layers.push_back(moss);
    m.procedural_params = {1.f, 2.f};
    return m;
}

/// Records uploads / releases (render thread) and hands out increasing GPU ids.
class RecordingSink final : public IRenderUploadSink {
public:
    RenderUploadResult upload(const RenderUploadCommand& command) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        RenderUploadResult r;
        if (command.payload == nullptr || command.payload->type() != command.type || failNext) {
            failNext = false;
            r.error = "refused";
            return r;
        }
        r.ok = true;
        r.gpu_resource = ++m_nextGpu;
        m_live.insert(r.gpu_resource);
        ++uploads;
        uploadThreads.insert(std::this_thread::get_id());
        return r;
    }
    void release(const RenderUploadCommand& command) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_live.erase(command.gpu_resource) == 0u) {
            ++badReleases;
        }
        ++releases;
    }
    usize liveCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_live.size();
    }
    bool isLive(u64 gpu) const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_live.count(gpu) != 0u;
    }
    u64 uploadCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return uploads;
    }
    u64 releaseCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return releases;
    }

    bool failNext = false;
    u64 uploads = 0;
    u64 releases = 0;
    u64 badReleases = 0;
    std::set<std::thread::id> uploadThreads;

private:
    mutable std::mutex m_mutex;
    u64 m_nextGpu = 1000;
    std::set<u64> m_live;
};

struct SchedulerScope {
    explicit SchedulerScope(u32 workers) {
        auto& scheduler = jobs::JobScheduler::instance();
        scheduler.shutdown();
        scheduler.initialize(workers);
    }
    ~SchedulerScope() { jobs::JobScheduler::instance().shutdown(); }
};

// ---- suites ----------------------------------------------------------------------------------------

int suite_id() {
    expect(normalize_asset_path("Game:\\Meshes\\.\\Rock\\..\\Cube.FUSEMESH") == "game:/meshes/cube.fusemesh",
           "normalise: case, backslashes, '.' and '..'");
    expect(normalize_asset_path("/a//b/") == "/a/b", "normalise: rooted, duplicate and trailing slashes");
    expect(normalize_asset_path("/../a") == "/a", "normalise: '..' never leaves a rooted path");
    expect(normalize_asset_path("game:/../a") == "game:/a", "normalise: '..' never removes a mount scheme");
    expect(normalize_asset_path("a/../../b") == "../b", "normalise: unmatched '..' kept on relative paths");
    expect(normalize_asset_path("") == "" && !asset_id_of("").valid(), "empty path has the invalid id");

    // FNV-1a 64 reference vectors (http://www.isthe.com/chongo/tech/comp/fnv/).
    static_assert(asset_id_from_normalized("a").value == 0xaf63dc4c8601ec8cull, "FNV-1a 64('a')");
    static_assert(asset_id_from_normalized("foobar").value == 0x85944171f73967e8ull, "FNV-1a 64('foobar')");
    expect(asset_id_of("Game:/Meshes/Cube.fusemesh") == asset_id_of("game:\\meshes\\cube.FUSEMESH"),
           "ids are equal for spellings of one path");
    expect(asset_id_of("game:/a.fusemesh") != asset_id_of("game:/b.fusemesh"), "different paths, different ids");
    expect(asset_id_of("game:/cube.fusemesh") == asset_id_from_normalized("game:/cube.fusemesh"),
           "asset_id_of hashes the normalised path");

    const AssetId id = asset_id_of("game:/meshes/cube.fusemesh");
    const std::string text = format_asset_id(id);
    AssetId parsed;
    expect(text.size() == 22u && text.rfind("asset:", 0) == 0, "text form is asset:<16 hex>");
    expect(parse_asset_id(text, parsed) && parsed == id, "format -> parse round trip");
    AssetId other;
    expect(parse_asset_id("0xAF63DC4C8601EC8C", other) && other.value == 0xaf63dc4c8601ec8cull, "0x form parses");
    expect(parse_asset_id("ff", other) && other.value == 0xffu, "bare hex parses");
    AssetId untouched = AssetId::fromValue(7u);
    expect(!parse_asset_id("asset:123", untouched) && !parse_asset_id("xyz", untouched) &&
               !parse_asset_id("", untouched) && !parse_asset_id("0x11112222333344445", untouched) &&
               untouched.value == 7u,
           "malformed text is refused and leaves the output alone");

    ecs::Mesh mesh{};
    ecs::MeshAssets assets{};
    expect(!assets.mesh.valid() && !assets.material.valid(), "ecs::MeshAssets ids default to invalid");
    ecs::Registry registry;
    const ecs::EntityID e = registry.create();
    mesh.material_id = 3u;
    registry.add<ecs::Mesh>(e, mesh);
    registry.add<ecs::MeshAssets>(e, ecs::MeshAssets{id, asset_id_of("game:/mat/rock.fusemat")});
    const ecs::Mesh* storedMesh = registry.get<ecs::Mesh>(e);
    const ecs::MeshAssets* stored = registry.get<ecs::MeshAssets>(e);
    expect(storedMesh != nullptr && storedMesh->material_id == 3u && stored != nullptr && stored->mesh == id &&
               stored->material == asset_id_of("game:/mat/rock.fusemat"),
           "an entity carries asset ids next to the numeric Mesh ids");
    static_assert(sizeof(ecs::Mesh) <= 60u, "ecs::Mesh hot row unchanged by the asset ids");
    return 0;
}

int suite_formats() {
    const CookedMesh quad = make_quad(2.f);
    std::vector<u8> bytes = fmsh_v1(quad);
    AssetPayload payload;
    std::string error;
    expect(decode_asset_payload(AssetType::Mesh, bytes.data(), bytes.size(), payload, &error), "FMSH v1 decodes: " + error);
    expect(payload.type() == AssetType::Mesh && payload.mesh() != nullptr && *payload.mesh() == quad,
           "decoded FMSH equals the source mesh");
    std::vector<u8> corrupt = bytes;
    corrupt[60] ^= 0x40u;
    expect(!decode_asset_payload(AssetType::Mesh, corrupt.data(), corrupt.size(), payload, &error) &&
               error.find("checksum") != std::string::npos,
           "flipped FMSH byte fails the checksum");
    CookedMesh truncated;
    expect(!deserialize_cooked_mesh(bytes.data(), bytes.size() - 3u, truncated, &error),
           "truncated FMSH is refused");

    const std::vector<u8> tex = fusetex_bc7(7u);
    CookedTexture texture;
    expect(parse_cooked_texture(tex.data(), tex.size(), texture, &error), "fusetex parses: " + error);
    expect(texture.format == BcFormat::BC7 && texture.width == 8u && texture.levels.size() == 2u &&
               texture.levels[0].blocks.size() == 64u && texture.levels[1].blocks.size() == 16u &&
               texture.levels[1].blocks[0] == static_cast<u8>(7u + 64u) && texture.srgb,
           "fusetex levels and blocks");
    std::vector<u8> badTex = tex;
    badTex.pop_back();
    expect(!parse_cooked_texture(badTex.data(), badTex.size(), texture, &error), "short fusetex block data is refused");
    expect(bc_mip_count(8u, 8u) == 4u && bc_block_count(5u, 5u) == 4u && bc_block_bytes(BcFormat::BC1) == 8u,
           "BCn helpers");

    const CookedMaterial mat = sample_material();
    std::vector<u8> matBytes = fusemat_bytes(mat);
    CookedMaterial readBack;
    expect(read_cooked_material(matBytes.data(), matBytes.size(), readBack, &error) && readBack == mat,
           "fusemat round trip: " + error);
    CookedMaterial bad = mat;
    bad.roughness = 2.f;
    const std::vector<u8> badBytes = fusemat_bytes(bad);
    expect(!read_cooked_material(badBytes.data(), badBytes.size(), readBack, &error) &&
               error.find("base.roughness") != std::string::npos,
           "out-of-range fusemat field is refused with its path");
    matBytes[0] = 'X';
    expect(!read_cooked_material(matBytes.data(), matBytes.size(), readBack, &error), "bad fusemat magic is refused");

    expect(asset_type_from_path("game:/X.FuseMesh") == AssetType::Mesh &&
               asset_type_from_path("a.fusetex") == AssetType::Texture &&
               asset_type_from_path("a.fusemat") == AssetType::Material &&
               asset_type_from_path("a.png") == AssetType::Unknown,
           "asset type from extension");
    return 0;
}

int suite_registry() {
    SchedulerScope scheduler(2);
    const fs::path dir = scratch_dir("registry");
    const CookedMesh quad = make_quad(3.f);
    write_file(dir / "Meshes" / "Quad.fusemesh", fmsh_v1(quad));
    write_file(dir / "tex" / "t.fusetex", fusetex_bc7(1u));
    write_file(dir / "mat" / "rock.fusemat", fusemat_bytes(sample_material()));
    std::vector<u8> broken = fmsh_v1(quad);
    broken[70] ^= 1u;
    write_file(dir / "broken.fusemesh", broken);
    write_file(dir / "notes.txt", {1u, 2u, 3u});

    io::VirtualFileSystem vfs;
    vfs.mount(io::MountKind::Game, dir.string(), "game:");
    RecordingSink sink;
    {
        AssetRegistry registry(vfs);
        const AssetId mesh = registry.acquire("game:/Meshes/Quad.fusemesh");
        const AssetId again = registry.acquire("GAME:/meshes/quad.FUSEMESH");
        expect(mesh.valid() && mesh == again, "two spellings share one id");
        expect(registry.refCount(mesh) == 2u && registry.stats().loadsStarted == 1u, "second acquire only adds a reference");
        expect(registry.pathOf(mesh) == "game:/Meshes/Quad.fusemesh", "path keeps its first spelling (the VFS read)");
        const AssetId tex = registry.acquire("game:/tex/t.fusetex");
        const AssetId mat = registry.acquire("game:/mat/rock.fusemat");

        expect(registry.pumpUntilIdle(sink, 10000), "loads complete");
        expect(registry.state(mesh) == AssetLoadState::Ready && registry.state(tex) == AssetLoadState::Ready &&
                   registry.state(mat) == AssetLoadState::Ready,
               "mesh, texture and material are Ready");
        expect(registry.mesh(mesh) != nullptr && *registry.mesh(mesh) == quad, "registry mesh equals the cooked data");
        expect(registry.texture(tex) != nullptr && registry.texture(tex)->levels.size() == 2u, "registry texture");
        expect(registry.material(mat) != nullptr && *registry.material(mat) == sample_material(), "registry material");
        const AssetRegistryStats s = registry.stats();
        expect(s.decodes == 3u && s.decodesOnJobWorker == 3u, "every decode ran on a JobScheduler worker (" +
                                                                  std::to_string(s.decodesOnJobWorker) + "/3)");
        expect(sink.uploadCount() == 3u, "three render uploads");
        const AssetHandle h = registry.handle(mesh);
        const u64 gpu = registry.gpuResource(mesh);
        expect(h.isValid() && registry.get(h) != nullptr && registry.get(h)->id == mesh && sink.isLive(gpu),
               "handle resolves to the committed asset with its GPU resource");

        expect(registry.release(mesh) == 1u && registry.state(mesh) == AssetLoadState::Ready, "one reference left");
        expect(registry.release(mesh) == 0u && registry.state(mesh) == AssetLoadState::Unloaded, "last release unloads");
        expect(!registry.handle(mesh).isValid() && registry.get(h) == nullptr && registry.mesh(mesh) == nullptr,
               "unloaded handle is stale (generation checked)");
        registry.drainRenderUploads(sink);
        expect(!sink.isLive(gpu) && sink.releaseCount() == 1u, "unload released the GPU copy on the render thread");
        expect(registry.release(mesh) == 0u, "release of an unreferenced id is a no-op");

        const AssetId reloaded = registry.acquire("game:/meshes/quad.fusemesh");
        expect(reloaded == mesh && registry.pumpUntilIdle(sink, 10000) && registry.state(mesh) == AssetLoadState::Ready &&
                   registry.stats().loadsStarted == 4u && *registry.mesh(mesh) == quad,
               "re-acquire loads again");
        expect(registry.handle(mesh) != h, "reload gets a new handle generation");

        const AssetId missing = registry.acquire("game:/nope.fusemesh");
        const AssetId unknown = registry.acquire("game:/notes.txt");
        const AssetId corrupt = registry.acquire("game:/broken.fusemesh");
        const AssetId unmounted = registry.acquire("other:/x.fusemesh");
        expect(registry.pumpUntilIdle(sink, 10000), "failing loads settle");
        expect(registry.state(missing) == AssetLoadState::Failed && !registry.error(missing).empty(), "missing file fails");
        expect(registry.state(unknown) == AssetLoadState::Failed &&
                   registry.error(unknown).find("unknown asset type") != std::string::npos,
               "unknown extension fails without a read");
        expect(registry.state(corrupt) == AssetLoadState::Failed &&
                   registry.error(corrupt).find("checksum") != std::string::npos,
               "corrupt file fails in decode: " + registry.error(corrupt));
        expect(registry.state(unmounted) == AssetLoadState::Failed, "unresolved mount fails");

        const AssetId byId = registry.registerPath("game:/tex/t.fusetex");
        expect(byId == tex && registry.acquire(byId) && registry.refCount(tex) == 2u, "acquire by registered id");
        expect(!registry.acquire(AssetId::fromValue(12345u)), "acquire of an unknown id fails");

        registry.unloadAll();
        registry.drainRenderUploads(sink);
        expect(registry.stats().ready == 0u && sink.liveCount() == 0u && sink.badReleases == 0u,
               "unloadAll frees every GPU copy exactly once");
    }
    {
        AssetRegistryConfig config;
        config.keepCpuData = false;
        AssetRegistry gpuOnly(vfs, config);
        const AssetId id = gpuOnly.acquire("game:/tex/t.fusetex");
        expect(gpuOnly.pumpUntilIdle(sink, 10000) && gpuOnly.state(id) == AssetLoadState::Ready &&
                   gpuOnly.texture(id) == nullptr && gpuOnly.gpuResource(id) != 0u,
               "keepCpuData=false drops the CPU copy after upload");
        gpuOnly.unloadAll();
        gpuOnly.drainRenderUploads(sink);
    }
    {
        AssetRegistry refusing(vfs);
        sink.failNext = true;
        const AssetId id = refusing.acquire("game:/tex/t.fusetex");
        expect(refusing.pumpUntilIdle(sink, 10000) && refusing.state(id) == AssetLoadState::Failed &&
                   refusing.error(id).find("render upload failed") != std::string::npos,
               "a refused upload fails the asset");
    }
    expect(sink.liveCount() == 0u, "no GPU copy leaked");
    vfs.waitIdle(5000);
    fs::remove_all(dir);
    return 0;
}

int suite_cancel() {
    SchedulerScope scheduler(2);
    const fs::path dir = scratch_dir("cancel");
    for (u32 i = 0; i < 4u; ++i) {
        write_file(dir / ("m" + std::to_string(i) + ".fusemesh"), fmsh_v1(make_quad(1.f + static_cast<f32>(i))));
    }
    io::VirtualFileSystem vfs;
    vfs.mount(io::MountKind::Game, dir.string(), "game:");
    RecordingSink sink;
    {
        AssetRegistry registry(vfs);
        // (a) cancelled while queued on the (suspended) I/O lane: never read, decoded or uploaded.
        vfs.suspend();
        const AssetId a = registry.acquire("game:/m0.fusemesh");
        expect(registry.state(a) == AssetLoadState::Queued, "suspended lane keeps the load queued");
        expect(registry.release(a) == 0u && registry.state(a) == AssetLoadState::Unloaded, "release cancels it");
        vfs.resume();
        expect(registry.pumpUntilIdle(sink, 5000) && vfs.waitIdle(5000), "idle after cancel");
        AssetRegistryStats s = registry.stats();
        expect(s.cancelled == 1u && s.decodes == 0u && sink.uploadCount() == 0u, "queued cancel: no decode, no upload");

        // (b) cancelled after decode, with its Upload command waiting for the render thread.
        const AssetId b = registry.acquire("game:/m1.fusemesh");
        expect(wait_until([&] { return registry.state(b) == AssetLoadState::Uploading; }, 5000), "b decoded");
        registry.release(b);
        registry.drainRenderUploads(sink);
        s = registry.stats();
        expect(s.staleUploadsDropped == 1u && sink.uploadCount() == 0u, "stale upload dropped on the render thread");

        // (c) cancelled after upload, before the game-thread commit: dropped at commit, GPU copy released.
        const AssetId c = registry.acquire("game:/m2.fusemesh");
        expect(wait_until([&] { return registry.state(c) == AssetLoadState::Uploading; }, 5000), "c decoded");
        registry.drainRenderUploads(sink);
        expect(registry.state(c) == AssetLoadState::Publishing && sink.liveCount() == 1u, "c uploaded, not committed");
        registry.release(c);
        expect(registry.update() == 0u && registry.stats().stalePublishesDropped == 1u, "stale publish dropped at commit");
        registry.drainRenderUploads(sink);
        expect(sink.liveCount() == 0u && sink.releaseCount() == 1u, "its GPU copy was released");

        // (d) released and re-acquired while the first load's upload is still queued: only the new
        //     load becomes Ready.
        const AssetId d = registry.acquire("game:/m3.fusemesh");
        expect(wait_until([&] { return registry.state(d) == AssetLoadState::Uploading; }, 5000), "d decoded");
        registry.release(d);
        registry.acquire("game:/m3.fusemesh");
        expect(registry.pumpUntilIdle(sink, 5000) && registry.state(d) == AssetLoadState::Ready &&
                   registry.mesh(d) != nullptr && registry.mesh(d)->bounds_max[0] == 4.f,
               "re-acquired asset is Ready");
        s = registry.stats();
        expect(s.staleUploadsDropped == 2u && sink.liveCount() == 1u, "stale first load dropped, one GPU copy live");
        registry.unloadAll();
        registry.drainRenderUploads(sink);
        expect(sink.liveCount() == 0u, "unloadAll released it");
    }
    {
        // (e) the registry is destroyed with loads queued and in flight; the lane then completes.
        vfs.suspend();
        {
            AssetRegistry doomed(vfs);
            for (u32 i = 0; i < 4u; ++i) {
                doomed.acquire("game:/m" + std::to_string(i) + ".fusemesh");
            }
        }
        vfs.resume();
        expect(vfs.waitIdle(5000), "lane idle after the registry went away");
        AssetRegistry busy(vfs);
        for (u32 i = 0; i < 4u; ++i) {
            busy.acquire("game:/m" + std::to_string(i) + ".fusemesh");
        }
    } // destroyed while decodes may run
    expect(vfs.waitIdle(5000), "lane idle after destroying a busy registry");
    fs::remove_all(dir);
    return 0;
}

int suite_mpsc() {
    // Queue level: 4 producers, 1 concurrent consumer.
    {
        constexpr u32 kProducers = 4;
        constexpr u32 kPerProducer = 20000;
        RenderUploadQueue queue;
        std::atomic<bool> go{false};
        std::vector<std::thread> producers;
        for (u32 p = 0; p < kProducers; ++p) {
            producers.emplace_back([&queue, &go, p] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (u32 i = 0; i < kPerProducer; ++i) {
                    RenderUploadCommand c;
                    c.id = AssetId::fromValue(p + 1u);
                    c.ticket = i;
                    c.gpu_resource = static_cast<u64>(p) * 1000000u + i;
                    queue.push(std::move(c));
                }
            });
        }
        std::vector<u64> nextExpected(kProducers, 0u);
        u64 popped = 0;
        bool ordered = true;
        std::thread consumer([&] {
            RenderUploadCommand c;
            while (popped < static_cast<u64>(kProducers) * kPerProducer) {
                if (!queue.tryPop(c)) {
                    std::this_thread::yield();
                    continue;
                }
                const u64 p = c.id.value - 1u;
                if (p >= kProducers || c.ticket != nextExpected[p] ||
                    c.gpu_resource != p * 1000000u + c.ticket) {
                    ordered = false;
                } else {
                    ++nextExpected[p];
                }
                ++popped;
            }
        });
        go.store(true, std::memory_order_release);
        for (std::thread& t : producers) {
            t.join();
        }
        consumer.join();
        RenderUploadCommand leftover;
        expect(ordered, "per-producer FIFO order and payload intact");
        expect(popped == static_cast<u64>(kProducers) * kPerProducer && !queue.tryPop(leftover) &&
                   queue.pushedCount() == queue.poppedCount(),
               "every command popped exactly once");
    }

    // Pipeline level: JobScheduler decode workers, 2 I/O threads, a render thread draining, the game
    // thread churning acquire / release / update.
    SchedulerScope scheduler(3);
    const fs::path dir = scratch_dir("mpsc");
    constexpr u32 kAssets = 32;
    std::vector<CookedMesh> meshes;
    for (u32 i = 0; i < kAssets; ++i) {
        meshes.push_back(make_quad(1.f + static_cast<f32>(i)));
        write_file(dir / ("m" + std::to_string(i) + ".fusemesh"), fmsh_v1(meshes.back()));
    }
    io::VirtualFileSystem vfs;
    io::IoLaneConfig lane;
    lane.ioThreadCount = 2;
    vfs.configureLane(lane);
    vfs.mount(io::MountKind::Game, dir.string(), "game:");
    RecordingSink sink;
    {
        AssetRegistry registry(vfs);
        std::atomic<bool> stop{false};
        std::thread renderThread([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (registry.drainRenderUploads(sink, 8) == 0u) {
                    std::this_thread::yield();
                }
            }
            registry.drainRenderUploads(sink);
        });
        std::vector<AssetId> ids(kAssets);
        bool allReady = true;
        bool dataOk = true;
        for (u32 round = 0; round < 8u; ++round) {
            for (u32 i = 0; i < kAssets; ++i) {
                ids[i] = registry.acquire("game:/m" + std::to_string(i) + ".fusemesh",
                                          static_cast<io::IoPriority>(i % io::kIoPriorityCount));
                if ((i + round) % 5u == 0u) {
                    registry.release(ids[i]); // cancel some immediately...
                    registry.acquire(ids[i]); // ...and want them again
                }
            }
            const bool ready = wait_until(
                [&] {
                    registry.update();
                    for (const AssetId id : ids) {
                        if (registry.state(id) != AssetLoadState::Ready) {
                            return false;
                        }
                    }
                    return true;
                },
                20000);
            allReady = allReady && ready;
            for (u32 i = 0; i < kAssets && ready; ++i) {
                const CookedMesh* m = registry.mesh(ids[i]);
                dataOk = dataOk && m != nullptr && *m == meshes[i];
            }
            // Release everything; half of them twice over two rounds via an extra reference.
            for (u32 i = 0; i < kAssets; ++i) {
                registry.release(ids[i]);
            }
        }
        expect(allReady, "every round reaches Ready under concurrency");
        expect(dataOk, "decoded meshes are intact under concurrency");
        registry.unloadAll();
        registry.update();
        stop.store(true, std::memory_order_release);
        renderThread.join();
        registry.update();
        registry.drainRenderUploads(sink);
        const AssetRegistryStats s = registry.stats();
        std::printf("mpsc pipeline: loads %llu, decodes %llu (on workers %llu), uploads %llu, releases %llu, "
                    "cancelled %llu, stale uploads %llu, stale publishes %llu\n",
                    static_cast<unsigned long long>(s.loadsStarted), static_cast<unsigned long long>(s.decodes),
                    static_cast<unsigned long long>(s.decodesOnJobWorker), static_cast<unsigned long long>(s.uploads),
                    static_cast<unsigned long long>(s.releases), static_cast<unsigned long long>(s.cancelled),
                    static_cast<unsigned long long>(s.staleUploadsDropped),
                    static_cast<unsigned long long>(s.stalePublishesDropped));
        expect(s.ready == 0u && s.inFlight == 0u, "nothing left loaded or in flight");
        expect(sink.liveCount() == 0u && sink.badReleases == 0u && sink.uploadCount() == sink.releaseCount(),
               "every GPU resource uploaded was released exactly once");
        expect(sink.uploadThreads.size() == 1u, "uploads ran on the render thread only");
    }
    vfs.waitIdle(5000);
    fs::remove_all(dir);
    return 0;
}

int suite_material() {
#if defined(FUSE_ASSET_TEST_HAS_FUSEMAT)
    namespace ml = fuse::renderer::material_layers;
    ml::FuseMat m;
    m.name = "rock/alpine/granite_a";
    m.shading = ml::FuseMatShading::Foliage;
    m.category = ml::FuseMatCategory::Stone;
    m.wind = ml::FuseMatWind::Leaves;
    m.albedo[0] = 0.3f;
    m.albedo[1] = 0.35f;
    m.albedo[2] = 0.4f;
    m.roughness = 0.8f;
    m.metallic = 0.1f;
    m.textures.albedo = "granite_albedo";
    m.textures.normal = "granite_normal";
    m.triplanar = true;
    m.triplanarSharpness = 6.f;
    m.stochastic = true;
    m.macroStrength = 0.3f;
    m.detail.albedo = "detail_a";
    m.detailFade[0] = 2.f;
    m.detailFade[1] = 40.f;
    ml::FuseMatLayer moss;
    moss.name = "moss";
    moss.mask = ml::kMlMaskSlopeUp;
    moss.albedo[0] = 0.2f;
    moss.albedo[1] = 0.3f;
    moss.albedo[2] = 0.1f;
    moss.coverage = 0.6f;
    moss.textures.albedo = "moss_albedo";
    ml::FuseMatLayer wet;
    wet.name = "wet";
    wet.mode = ml::kMlLayerWet;
    wet.albedo[0] = 0.5f;
    wet.albedo[1] = 0.5f;
    wet.albedo[2] = 0.5f;
    m.layers = {moss, wet};
    m.proceduralFunction = 3u;
    m.proceduralParams = {0.5f, 1.5f, 2.5f};
    expect(ml::validate_fusemat(m).ok, "renderer accepts the sample material");

    const std::vector<u8> bytes = ml::write_fusemat_binary(m);
    ml::FuseMat ref;
    expect(ml::read_fusemat_binary(bytes.data(), bytes.size(), ref).ok, "renderer reads its own binary");
    CookedMaterial got;
    std::string error;
    expect(read_cooked_material(bytes.data(), bytes.size(), got, &error), "runtime reads the renderer's binary: " + error);
    bool same = got.version == ref.version && got.name == ref.name &&
                got.shading == static_cast<u32>(ref.shading) && got.category == static_cast<u32>(ref.category) &&
                got.wind == static_cast<u32>(ref.wind) && std::memcmp(got.albedo, ref.albedo, sizeof(got.albedo)) == 0 &&
                got.roughness == ref.roughness && got.metallic == ref.metallic &&
                got.normal_strength == ref.normalStrength && got.textures.albedo == ref.textures.albedo &&
                got.textures.normal == ref.textures.normal && got.uv_scale == ref.uvScale &&
                got.triplanar == ref.triplanar && got.triplanar_sharpness == ref.triplanarSharpness &&
                got.stochastic == ref.stochastic && got.stochastic_lattice == ref.stochasticLattice &&
                got.macro_scale == ref.macroScale && got.macro_strength == ref.macroStrength &&
                got.detail.albedo == ref.detail.albedo && got.detail.normal == ref.detail.normal &&
                got.detail_scale == ref.detailScale && got.detail_strength == ref.detailStrength &&
                got.detail_fade[0] == ref.detailFade[0] && got.detail_fade[1] == ref.detailFade[1] &&
                got.layers.size() == ref.layers.size() && got.procedural_function == ref.proceduralFunction &&
                got.procedural_params == ref.proceduralParams;
    for (usize i = 0; same && i < got.layers.size(); ++i) {
        const CookedMaterialLayer& a = got.layers[i];
        const ml::FuseMatLayer& b = ref.layers[i];
        same = a.name == b.name && a.mode == static_cast<u32>(b.mode) && a.mask == static_cast<u32>(b.mask) &&
               a.mask_bias == b.maskBias && a.mask_scale == b.maskScale && a.coverage == b.coverage &&
               a.contrast == b.contrast && std::memcmp(a.albedo, b.albedo, sizeof(a.albedo)) == 0 &&
               a.roughness == b.roughness && a.metallic == b.metallic && a.uv_scale == b.uvScale &&
               a.normal_strength == b.normalStrength && a.textures.albedo == b.textures.albedo &&
               a.textures.normal == b.textures.normal;
    }
    expect(same, "runtime material equals the renderer's read_fusemat_binary field for field");
    expect(fusemat_bytes(got) == bytes, "runtime layout description reproduces the renderer's bytes");

    ml::FuseMat invalid = m;
    invalid.layers[0].contrast = 100.f;
    const std::vector<u8> invalidBytes = ml::write_fusemat_binary(invalid);
    expect(!ml::read_fusemat_binary(invalidBytes.data(), invalidBytes.size(), ref).ok &&
               !read_cooked_material(invalidBytes.data(), invalidBytes.size(), got, &error) &&
               error.find("layers[0].contrast") != std::string::npos,
           "both readers refuse the same out-of-range field");
    return 0;
#else
    std::printf("SKIP: renderer .fusemat library not in this build\n");
    return 77;
#endif
}

int suite_cook(const char* cookTool) {
#if defined(FUSE_ASSET_TEST_HAS_COOK)
    const fs::path src = fs::path(FUSE_SOURCE_DIR) / "vendor/assimp/test/models/glTF2/BoxTextured-glTF";
    if (!fs::exists(src / "BoxTextured.gltf")) {
        std::printf("SKIP: %s not found\n", (src / "BoxTextured.gltf").string().c_str());
        return 77;
    }
    const fs::path dir = scratch_dir("cook");
    fs::create_directories(dir / "src");
    for (const char* name : {"BoxTextured.gltf", "BoxTextured0.bin", "CesiumLogoFlat.png"}) {
        fs::copy_file(src / name, dir / "src" / name, fs::copy_options::overwrite_existing);
    }
    const fs::path cooked = dir / "cooked";
    fs::create_directories(cooked);
    const std::string meshOut = (cooked / "box.fusemesh").string();
    const std::string texOut = (cooked / "logo.fusetex").string();
    if (cookTool != nullptr) {
        // The fuse_cook tool (Tools/FUSE/fuse_cook.cpp) writes its cook cache into the working directory.
        const std::string prefix = "cd \"" + dir.string() + "\" && \"" + cookTool + "\"";
        const std::string meshCmd = prefix + " --mesh --input \"" + (dir / "src" / "BoxTextured.gltf").string() +
                                    "\" --output \"" + meshOut + "\"";
        const std::string texCmd = prefix + " --texture --input \"" + (dir / "src" / "CesiumLogoFlat.png").string() +
                                   "\" --output \"" + texOut + "\"";
        std::printf("cook: %s\n", meshCmd.c_str());
        const int meshRc = std::system(meshCmd.c_str());
        std::printf("cook: %s\n", texCmd.c_str());
        const int texRc = std::system(texCmd.c_str());
        expect(meshRc == 0 && texRc == 0, "fuse_cook cooks the glTF cube and the PNG");
    } else {
        const cook::CookStubWriteResult m = cook::cook_mesh_file((dir / "src" / "BoxTextured.gltf").string(), meshOut);
        if (m.failure == cook::CookFailure::ImporterUnavailable) {
            std::printf("SKIP: mesh importer unavailable (%s)\n", m.note.c_str());
            return 77;
        }
        cook::TextureCookOptions options;
        const cook::CookStubWriteResult t =
            cook::cook_texture_file((dir / "src" / "CesiumLogoFlat.png").string(), texOut, options);
        expect(m.ok && t.ok, "cook library cooks the glTF cube and the PNG: " + m.note + " / " + t.note);
    }
    // FMSH v2 with every W0.2 feature the build supports: exercises the moved section reader.
    const std::string v2Out = (cooked / "box_v2.fusemesh").string();
    cook::MeshCookOptions v2;
    v2.import_tangents = true;
    v2.import_material_names = true;
    v2.encoding.quantize_normals = true;
    v2.encoding.meshopt_codec = cook::mesh_optimizer_available();
    v2.optimize.lods = cook::mesh_optimizer_available();
    v2.optimize.meshlets = cook::mesh_meshlets_available();
    v2.optimize.cluster_dag = cook::mesh_meshlets_available();
    const cook::CookStubWriteResult v2Result = cook::cook_mesh_file((dir / "src" / "BoxTextured.gltf").string(), v2Out, v2);
    expect(v2Result.ok, "FMSH v2 cook: " + v2Result.note);

    cook::CookedMesh refMesh;
    cook::CookedMesh refV2;
    cook::CookedTexture refTex;
    std::string error;
    expect(cook::load_cooked_mesh(meshOut, refMesh, &error), "cook reader loads the mesh: " + error);
    expect(cook::load_cooked_mesh(v2Out, refV2, &error), "cook reader loads the v2 mesh: " + error);
    expect(cook::load_cooked_texture(texOut, refTex, &error), "cook reader loads the texture: " + error);
    expect(refMesh.vertex_count() == 24u && refMesh.indices.size() == 36u, "BoxTextured: 24 vertices, 12 triangles");
    expect(!refTex.levels.empty() && refTex.width > 0u, "PNG cooked to a BCn texture");
    std::printf("cooked: mesh %u v / %zu i, v2 lods %zu meshlets %zu dag groups %zu, texture %ux%u %s %zu mips\n",
                refMesh.vertex_count(), refMesh.indices.size(), refV2.lods.size(), refV2.meshlets.meshlets.size(),
                refV2.cluster_dag.groups.size(), refTex.width, refTex.height, refTex.compression.c_str(),
                refTex.levels.size());

    SchedulerScope scheduler(2);
    io::VirtualFileSystem vfs;
    vfs.mount(io::MountKind::Game, cooked.string(), "game:");
    RecordingSink sink;
    {
        AssetRegistry registry(vfs);
        const AssetId mesh = registry.acquire("game:/box.fusemesh");
        const AssetId meshV2 = registry.acquire("game:/box_v2.fusemesh");
        const AssetId tex = registry.acquire("game:/logo.fusetex");
        expect(registry.pumpUntilIdle(sink, 20000), "cooked assets load");
        expect(registry.state(mesh) == AssetLoadState::Ready && registry.state(meshV2) == AssetLoadState::Ready &&
                   registry.state(tex) == AssetLoadState::Ready,
               "cooked assets are Ready: " + registry.error(mesh) + registry.error(meshV2) + registry.error(tex));
        expect(registry.mesh(mesh) != nullptr && *registry.mesh(mesh) == refMesh,
               "runtime mesh equals the cook library's reader output");
        expect(registry.mesh(meshV2) != nullptr && *registry.mesh(meshV2) == refV2,
               "runtime FMSH v2 mesh (streams + sections) equals the cook library's reader output");
        expect(registry.texture(tex) != nullptr && *registry.texture(tex) == refTex,
               "runtime texture equals the cook library's reader output");
        expect(sink.uploadCount() == 3u, "three uploads");
        registry.unloadAll();
        registry.drainRenderUploads(sink);
        expect(sink.liveCount() == 0u, "cooked assets unloaded");
    }
    vfs.waitIdle(5000);
    fs::remove_all(dir);
    return 0;
#else
    (void)cookTool;
    std::printf("SKIP: Tools/FUSE/Cook not in this build\n");
    return 77;
#endif
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    int rc = 0;
    auto run = [&](const char* name, int (*fn)()) {
        if (suite == name || suite == "all") {
            const int r = fn();
            if (r == 77 && suite != "all") {
                rc = 77;
            }
        }
    };
    run("id", suite_id);
    run("formats", suite_formats);
    run("registry", suite_registry);
    run("cancel", suite_cancel);
    run("mpsc", suite_mpsc);
    run("material", suite_material);
    if (suite == "cook" || suite == "all") {
        const int r = suite_cook(argc > 2 ? argv[2] : nullptr);
        if (r == 77 && suite == "cook") {
            rc = 77;
        }
    }
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    if (rc == 77) {
        return 77;
    }
    std::printf("fuse_asset_runtime %s: OK\n", suite.c_str());
    return 0;
}
