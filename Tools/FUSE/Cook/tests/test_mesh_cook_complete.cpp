// E05 mesh cook completeness gate (RE-P1-7, GREP-COOK-1; docs/unification/RENDERER-EXECUTION.md,
// docs/plans/FUSE_ASSET_PLAN.md W0.2). CPU only. Cooks a repository sample mesh through AssetCooker (the
// path fuse_cook and the import pipeline use) with lod_count / compress / meshlets / DAG / pages and checks:
//   - FMSH v2 with N LOD levels (N - 1 LOD ranges per submesh + LOD 0), quantised position / normal
//     streams (stream table formats), MSHL + DAG sections;
//   - the `.fusepages` next to the FMSH parses, binds to the FMSH DAG (links hash + counts) and fully
//     validates against the DAG rebuilt from the source (validate_cluster_page_file);
//   - the cook cache: an identical cook is a hit, changing any of the new flags (or page_bytes) is a
//     miss, a deleted / stale `.fusepages` forces a re-cook, and a cook without pages removes the old one;
//   - GREP-COOK-1 lenient path: write_mesh_stub honours lod_count / compressed;
//   - a dense generated mesh pages into several pages with dependencies; too-small pages fail the cook
//     without writing anything;
//   - optional argv[1]: the fuse_cook CLI (--lods / --compress / --pages) produces the same outputs.
#include <fuse/asset/detail/fmsh_layout.hpp>
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/renderer/geometry_streaming/cluster_page_file.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace fuse;
namespace fs = std::filesystem;
namespace gs = fuse::renderer::geometry_streaming;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::vector<u8> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

u32 le32(const std::vector<u8>& b, usize at) {
    return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) | (static_cast<u32>(b[at + 2]) << 16) |
           (static_cast<u32>(b[at + 3]) << 24);
}

struct StreamEntry {
    u32 semantic = 0;
    u32 format = 0;
};

/// FMSH v2 header fields + stream table (no meshopt codec, no material slots: what these cooks write).
bool read_v2_streams(const std::vector<u8>& b, u32& version, u32& flags, std::vector<StreamEntry>& streams) {
    streams.clear();
    if (b.size() < asset::detail::kFmshHeaderBytesV2) {
        return false;
    }
    version = le32(b, 4);
    flags = le32(b, 8);
    if (version != 2u) {
        return false;
    }
    const u32 vertexCount = le32(b, 12);
    const u32 submeshCount = le32(b, 20);
    const u32 streamCount = le32(b, 48);
    const u32 slotCount = le32(b, 52);
    if (slotCount != 0u || (flags & asset::detail::kFmshFlagMeshoptCodec) != 0u) {
        return false;
    }
    usize at = asset::detail::kFmshHeaderBytesV2 + static_cast<usize>(submeshCount) * asset::detail::kFmshSubmeshBytes;
    for (u32 s = 0; s < streamCount; ++s) {
        if (at + 12u > b.size()) {
            return false;
        }
        StreamEntry e{le32(b, at), le32(b, at + 4)};
        const u32 bytes = le32(b, at + 8);
        if (bytes != vertexCount * asset::detail::stream_element_bytes(static_cast<asset::MeshStreamFormat>(e.format))) {
            return false;
        }
        streams.push_back(e);
        at += 12u + ((bytes + 3u) & ~3u);
    }
    return true;
}

u32 stream_format(const std::vector<StreamEntry>& streams, asset::MeshStreamSemantic semantic) {
    for (const StreamEntry& e : streams) {
        if (e.semantic == static_cast<u32>(semantic)) {
            return e.format;
        }
    }
    return 0u;
}

/// The source mesh exactly as the cook builds it before serializing (import + LODs + meshlets + DAG).
bool rebuild_source_mesh(const std::string& input, const project::MeshImportDesc& desc, cook::CookedMesh& mesh) {
    cook::MeshCookOptions options;
    options.generate_normals = desc.generate_normals;
    std::string error;
    if (!cook::import_mesh_file(input, options, mesh, &error)) {
        std::fprintf(stderr, "  import: %s\n", error.c_str());
        return false;
    }
    cook::MeshLodOptions lod;
    if (desc.generate_lods && cook::lod_options_for_count(desc.lod_count, lod) && !cook::build_mesh_lods(mesh, lod, &error)) {
        std::fprintf(stderr, "  lods: %s\n", error.c_str());
        return false;
    }
    if (!cook::build_mesh_meshlets(mesh, true, &error)) {
        std::fprintf(stderr, "  meshlets: %s\n", error.c_str());
        return false;
    }
    return true;
}

/// Torus primitive (1276 triangles, 2 submeshes): simplifies to a full 4-level chain. (The sphere / cube /
/// cylinder primitives are imported with every vertex on a UV / normal seam, so meshopt cannot collapse
/// anything and the §5.3 rule keeps no LOD level; the cook handles that by writing LOD 0 only.)
std::string sample_mesh() {
    return std::string(FUSE_SOURCE_DIR) + "/Templates/BaseGame/game/data/Prototyping/shapes/Primitives/TorusPrimitive.fbx";
}

/// Dense, non-planar heightfield grid (OBJ) so the DAG has several levels and the page file several pages.
std::string write_dense_grid(const fs::path& dir, u32 cells) {
    const std::string path = (dir / "dense_grid.obj").string();
    std::ofstream out(path);
    for (u32 y = 0; y <= cells; ++y) {
        for (u32 x = 0; x <= cells; ++x) {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(cells);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(cells);
            const f32 h = 0.08f * std::sin(fx * 9.f) * std::cos(fy * 7.f) + 0.03f * std::sin((fx + fy) * 23.f);
            out << "v " << fx << ' ' << h << ' ' << fy << '\n';
            out << "vt " << fx << ' ' << fy << '\n';
        }
    }
    const u32 row = cells + 1u;
    for (u32 y = 0; y < cells; ++y) {
        for (u32 x = 0; x < cells; ++x) {
            const u32 a = y * row + x + 1u;
            const u32 b = a + 1u;
            const u32 c = a + row;
            const u32 d = c + 1u;
            out << "f " << a << '/' << a << ' ' << c << '/' << c << ' ' << b << '/' << b << '\n';
            out << "f " << b << '/' << b << ' ' << c << '/' << c << ' ' << d << '/' << d << '\n';
        }
    }
    return path;
}

project::MeshImportDesc full_desc(const std::string& input, const std::string& output) {
    project::MeshImportDesc desc;
    desc.input_path = input;
    desc.output_path = output;
    desc.generate_lods = true;
    desc.lod_count = 4;
    desc.compress = true;
    desc.cluster_pages = true;
    return desc;
}

void check_lod_mapping() {
    cook::MeshLodOptions lod;
    check(!cook::lod_options_for_count(0, lod) && lod.ratios.empty(), "lod_count 0: no LOD chain");
    check(!cook::lod_options_for_count(1, lod) && lod.ratios.empty(), "lod_count 1: LOD 0 only");
    check(cook::lod_options_for_count(4, lod) && lod.ratios == std::vector<f32>({0.5f, 0.25f, 0.1f}),
          "lod_count 4: the MeshLodOptions default ratios");
    check(cook::lod_options_for_count(6, lod) && lod.ratios == std::vector<f32>({0.5f, 0.25f, 0.1f, 0.05f, 0.025f}),
          "lod_count 6: defaults, then halving");
    check(cook::lod_options_for_count(2, lod) && lod.ratios == std::vector<f32>({0.5f}), "lod_count 2: one ratio");
    check(cook::cluster_pages_path("a/b.fusemesh") == fs::path("a/b.fusepages").string(), "pages path next to the FMSH");
}

void check_sample_cook(const fs::path& dir) {
    const std::string input = sample_mesh();
    if (!fs::exists(input)) {
        check(false, "repository sample mesh missing: " + input);
        return;
    }
    const std::string output = (dir / "torus.fusemesh").string();
    const std::string pages = cook::cluster_pages_path(output);
    const project::MeshImportDesc desc = full_desc(input, output);

    project::AssetCooker cooker;
    const project::CookRecord record = cooker.cook_mesh(desc);
    std::printf("  sample cook: %s\n", record.note.c_str());
    check(record.ok && !record.cache_hit, "sample mesh cooks (lods 4, compress, pages)");
    if (!record.ok) {
        return;
    }

    // FMSH v2: header, quantised stream formats.
    const std::vector<u8> bytes = read_bytes(output);
    u32 version = 0;
    u32 flags = 0;
    std::vector<StreamEntry> streams;
    check(read_v2_streams(bytes, version, flags, streams), "FMSH v2 stream table readable");
    check(version == 2u, "cooked mesh is FMSH v2");
    check((flags & asset::detail::kFmshFlagQuantizedPositions) != 0u, "quantised-positions flag set");
    check((flags & asset::detail::kFmshFlagSections) != 0u, "sections flag set");
    check(stream_format(streams, asset::MeshStreamSemantic::Position) == static_cast<u32>(asset::MeshStreamFormat::Unorm16x3),
          "position stream is Unorm16x3");
    check(stream_format(streams, asset::MeshStreamSemantic::Normal) == static_cast<u32>(asset::MeshStreamFormat::OctSnorm16x2),
          "normal stream is OctSnorm16x2");

    cook::CookedMesh loaded;
    std::string error;
    check(cook::load_cooked_mesh(output, loaded, &error), "cooked FMSH loads: " + error);
    // N LOD levels: LOD 0 + 3 LOD ranges (one per submesh each).
    check(loaded.lods.size() == 3u, "FMSH carries lod_count - 1 = 3 LOD levels (got " + std::to_string(loaded.lods.size()) + ")");
    for (usize l = 0; l < loaded.lods.size(); ++l) {
        check(loaded.lods[l].ranges.size() == loaded.submeshes.size(), "every LOD level has one range per submesh");
    }
    check(!loaded.meshlets.empty(), "FMSH carries the MSHL meshlet table");
    check(!loaded.cluster_dag.empty() && loaded.cluster_dag.leaf_cluster_count == loaded.meshlets.meshlets.size(),
          "FMSH carries the cluster DAG over its meshlets");

    // .fusepages next to the FMSH: parses, binds to the loaded FMSH, validates against the DAG.
    check(fs::exists(pages), ".fusepages written next to the FMSH");
    check(cook::cluster_pages_bind_to_mesh(loaded, pages, &error), "pages bind to the loaded FMSH DAG: " + error);
    gs::ClusterPageFile file;
    check(gs::load_cluster_page_file(pages, file, &error), "pages parse: " + error);
    check(file.page_count() >= 1u && file.cluster_count == loaded.cluster_dag.cluster_count(), "page file covers every cluster");
    cook::CookedMesh source;
    if (rebuild_source_mesh(input, desc, source)) {
        check(source.meshlets == loaded.meshlets && source.cluster_dag == loaded.cluster_dag,
              "FMSH meshlet / DAG sections equal the source rebuild");
        check(cook::validate_mesh_cluster_pages(source, pages, &error),
              ".fusepages validates against the DAG (validate_cluster_page_file): " + error);
    } else {
        check(false, "source rebuild for validation");
    }

    // Cache: identical cook hits; each changed flag misses.
    const project::CookRecord again = cooker.cook_mesh(desc);
    check(again.ok && again.cache_hit, "identical cook is a cache hit");

    project::MeshImportDesc changed = desc;
    changed.page_bytes = 32u * 1024u;
    const project::CookRecord pageBytesMiss = cooker.cook_mesh(changed);
    check(pageBytesMiss.ok && !pageBytesMiss.cache_hit, "changing page_bytes invalidates the cache entry");
    check(pageBytesMiss.content_hash != again.content_hash, "page_bytes is part of the cache key");

    changed = desc;
    changed.compress = false;
    const project::CookRecord compressMiss = cooker.cook_mesh(changed);
    check(compressMiss.ok && !compressMiss.cache_hit, "changing compress invalidates the cache entry");

    changed = desc;
    changed.lod_count = 3;
    const project::CookRecord lodMiss = cooker.cook_mesh(changed);
    check(lodMiss.ok && !lodMiss.cache_hit, "changing lod_count invalidates the cache entry");
    if (lodMiss.ok) {
        cook::CookedMesh m;
        check(cook::load_cooked_mesh(output, m) && m.lods.size() == 2u, "lod_count 3 cooks 2 LOD levels");
    }

    changed = desc;
    changed.cluster_pages = false;
    changed.cluster_dag = true;
    const project::CookRecord dagOnly = cooker.cook_mesh(changed);
    check(dagOnly.ok && !dagOnly.cache_hit, "turning pages off invalidates the cache entry");
    check(!fs::exists(pages), "a cook without pages removes the stale .fusepages");
    if (dagOnly.ok) {
        cook::CookedMesh m;
        check(cook::load_cooked_mesh(output, m) && !m.cluster_dag.empty(), "--dag alone still writes the DAG");
    }

    changed.cluster_dag = false;
    changed.meshlets = true;
    const project::CookRecord meshletOnly = cooker.cook_mesh(changed);
    check(meshletOnly.ok && !meshletOnly.cache_hit, "DAG -> meshlets only invalidates the cache entry");
    if (meshletOnly.ok) {
        cook::CookedMesh m;
        check(cook::load_cooked_mesh(output, m) && !m.meshlets.empty() && m.cluster_dag.empty(), "meshlets without a DAG");
    }

    // Back to the full cook: output was overwritten meanwhile, so the (still cached) full entry must not be
    // reused as is: the FMSH on disk no longer carries the DAG the key describes... the cooker re-checks
    // the pages binding, which fails (no .fusepages), and re-cooks.
    const project::CookRecord full = cooker.cook_mesh(desc);
    check(full.ok && !full.cache_hit, "full cook after other cooks of the same output re-cooks (pages missing)");
    check(fs::exists(pages), "re-cook restores the .fusepages");
    const project::CookRecord hit = cooker.cook_mesh(desc);
    check(hit.ok && hit.cache_hit, "then hits again");

    // Stale detection: a .fusepages that no longer binds (here: from another mesh / truncated) forces a re-cook.
    {
        std::vector<u8> truncated = read_bytes(pages);
        truncated.resize(truncated.size() / 2u);
        std::ofstream(pages, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(truncated.data()), static_cast<std::streamsize>(truncated.size()));
    }
    const project::CookRecord stale = cooker.cook_mesh(desc);
    check(stale.ok && !stale.cache_hit, "a damaged .fusepages forces a re-cook");
    cook::CookedMesh after;
    check(cook::load_cooked_mesh(output, after) && cook::cluster_pages_bind_to_mesh(after, pages),
          "re-cooked .fusepages binds again");
    fs::remove(pages);
    const project::CookRecord missing = cooker.cook_mesh(desc);
    check(missing.ok && !missing.cache_hit && fs::exists(pages), "a deleted .fusepages forces a re-cook");

    // Determinism: a fresh cooker writes identical FMSH + page bytes.
    const std::vector<u8> fmshA = read_bytes(output);
    const std::vector<u8> pagesA = read_bytes(pages);
    project::AssetCooker fresh;
    const project::CookRecord again2 = fresh.cook_mesh(desc);
    check(again2.ok && read_bytes(output) == fmshA && read_bytes(pages) == pagesA, "cook output is deterministic");
}

void check_lenient_stub_path(const fs::path& dir) {
    // GREP-COOK-1: write_mesh_stub (the lenient cook) honours lod_count / compressed.
    const std::string input = sample_mesh();
    const std::string output = (dir / "torus_lenient.fusemesh").string();
    const cook::CookStubWriteResult written = cook::write_mesh_stub(input, output, 3u, true);
    check(written.ok, "lenient mesh cook writes: " + written.note);
    cook::CookedMesh mesh;
    check(cook::load_cooked_mesh(output, mesh), "lenient output is a real FMSH (not a text stub)");
    check(mesh.lods.size() == (cook::mesh_optimizer_available() ? 2u : 0u), "lenient lod_count 3 -> 2 LOD levels");
    const std::vector<u8> bytes = read_bytes(output);
    u32 version = 0;
    u32 flags = 0;
    std::vector<StreamEntry> streams;
    check(read_v2_streams(bytes, version, flags, streams) &&
              stream_format(streams, asset::MeshStreamSemantic::Position) == static_cast<u32>(asset::MeshStreamFormat::Unorm16x3),
          "lenient compressed -> quantised positions");

    const std::string plain = (dir / "torus_plain.fusemesh").string();
    check(cook::write_mesh_stub(input, plain, 0u, false).ok, "lenient plain cook");
    check(read_bytes(plain).size() > 8u && le32(read_bytes(plain), 4) == 1u, "lod_count 0 / uncompressed stays FMSH v1");

    project::AssetCooker lenient;
    lenient.set_import_validation(project::ImportValidation::Lenient);
    project::MeshImportDesc desc = full_desc(input, (dir / "torus_lenient_pages.fusemesh").string());
    const project::CookRecord record = lenient.cook_mesh(desc);
    check(record.ok && fs::exists(cook::cluster_pages_path(desc.output_path)), "lenient cooker honours pages too");
}

void check_dense_grid(const fs::path& dir) {
    const std::string input = write_dense_grid(dir, 96u);
    const std::string output = (dir / "dense_grid.fusemesh").string();
    project::MeshImportDesc desc = full_desc(input, output);
    desc.lod_count = 1; // pages only
    desc.compress = false;
    project::AssetCooker cooker;
    const project::CookRecord record = cooker.cook_mesh(desc);
    std::printf("  dense grid cook: %s\n", record.note.c_str());
    check(record.ok, "dense grid cooks with pages");
    if (!record.ok) {
        return;
    }
    const std::string pages = cook::cluster_pages_path(output);
    gs::ClusterPageFile file;
    std::string error;
    check(gs::load_cluster_page_file(pages, file, &error), "dense pages parse: " + error);
    usize deps = file.page_deps.size();
    std::printf("  dense grid: %u pages (%u coarse), %u groups, %u clusters, %zu dependencies\n", file.page_count(),
                file.coarse_page_count, file.group_count, file.cluster_count, deps);
    check(file.page_count() > 1u, "dense grid spans several pages");
    check(deps > 0u, "fine pages depend on coarser ones");
    check(file.cluster_count > file.leaf_cluster_count, "the DAG has LOD clusters");
    cook::CookedMesh source;
    if (rebuild_source_mesh(input, desc, source)) {
        check(cook::validate_mesh_cluster_pages(source, pages, &error), "dense .fusepages validates against the DAG: " + error);
    }

    // Too-small pages: the cook fails and writes neither file.
    const std::string tinyOut = (dir / "dense_grid_tiny_pages.fusemesh").string();
    fs::remove(tinyOut);
    fs::remove(cook::cluster_pages_path(tinyOut));
    project::MeshImportDesc tiny = desc;
    tiny.output_path = tinyOut;
    tiny.page_bytes = 1024u;
    const project::CookRecord failed = cooker.cook_mesh(tiny);
    check(!failed.ok && failed.status == project::CookStatus::InvalidGeometry, "a group larger than a page fails the cook");
    check(!fs::exists(tinyOut) && !fs::exists(cook::cluster_pages_path(tinyOut)), "a failed page build writes nothing");
}

/// std::system in `dir` (cmd.exe strips one pair of outer quotes, so the command is wrapped on Windows).
int run_in(const fs::path& dir, const std::string& command) {
#if defined(_WIN32)
    const std::string line = "\"cd /d \"" + dir.string() + "\" && " + command + "\"";
#else
    const std::string line = "cd \"" + dir.string() + "\" && " + command;
#endif
    return std::system(line.c_str());
}

void check_cli(const fs::path& dir, const std::string& fuseCook) {
    const std::string output = (dir / "cli_torus.fusemesh").string();
    fs::remove(output);
    fs::remove(cook::cluster_pages_path(output));
    const std::string command = "\"" + fuseCook + "\" --mesh --input \"" + sample_mesh() + "\" --output \"" + output +
                                "\" --lods 4 --compress --pages";
    const int rc = run_in(dir, command);
    check(rc == 0, "fuse_cook --mesh --lods 4 --compress --pages exits 0");
    cook::CookedMesh mesh;
    check(cook::load_cooked_mesh(output, mesh) && mesh.lods.size() == 3u && !mesh.cluster_dag.empty(),
          "CLI output carries LODs + DAG");
    check(cook::cluster_pages_bind_to_mesh(mesh, cook::cluster_pages_path(output)), "CLI .fusepages binds");
    const std::vector<u8> reference = read_bytes((dir / "torus.fusemesh").string());
    check(read_bytes(output) == reference, "CLI FMSH equals the AssetCooker FMSH for the same flags");
    const std::string bad = "\"" + fuseCook + "\" --mesh --input x --output y --lods nope > cli_bad.log 2>&1";
    check(run_in(dir, bad) != 0, "--lods rejects a non-number");
}

} // namespace

int main(int argc, char** argv) {
    if (!cook::mesh_optimizer_available() || !cook::mesh_meshlets_available() || !cook::mesh_cluster_pages_available()) {
        std::printf("fuse_asset_mesh_cook_complete: meshoptimizer / renderer geometry not linked, skipped\n");
        return 77;
    }
    const fs::path dir = fs::path(FUSE_TEST_OUT_DIR) / "mesh_cook_complete";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    check_lod_mapping();
    check_sample_cook(dir);
    check_lenient_stub_path(dir);
    check_dense_grid(dir);
    if (argc > 1) {
        check_cli(dir, argv[1]);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_mesh_cook_complete: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_asset_mesh_cook_complete: OK\n");
    return 0;
}
