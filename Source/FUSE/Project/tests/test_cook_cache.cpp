#include <fuse/core/init.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_content_hash.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string writeTempFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return path;
}

void testCombineCookCacheKeyGuards() {
    expectTrue(fuse::project::combine_cook_cache_key(0, 0) == 0, "all-zero cache key stays zero");
    expectTrue(fuse::project::combine_cook_cache_key(0, 42u) == 0,
               "zero source hash rejects non-zero upstream fold");
    expectTrue(fuse::project::combine_cook_cache_key(99u, 0) == 99u, "valid source hash passes through alone");
    expectTrue(fuse::project::combine_cook_cache_key(99u, 42u) != 99u,
               "non-zero upstream alters combined cache key");

    expectTrue(!fuse::project::is_cacheable_cook_cache_key(0, 0), "zero fold is not cacheable");
    expectTrue(!fuse::project::is_cacheable_cook_cache_key(0, 42u), "zero source fold is not cacheable");
    expectTrue(fuse::project::is_cacheable_cook_cache_key(99u, 0), "valid source-only fold is cacheable");
    expectTrue(fuse::project::is_cacheable_cook_cache_key(99u, 42u), "valid combined fold is cacheable");
}

void testCookCacheEmptyPathRejection() {
    expectTrue(!fuse::project::is_valid_cook_cache_path(""), "empty path fails path validation");
    expectTrue(fuse::project::is_valid_cook_cache_path("/tmp/fuse_b79_cache_ok.obj"),
               "non-empty path passes validation");
    expectTrue(fuse::project::hash_file_content("") == 0, "empty path hashes to zero");

    fuse::project::MeshImportDesc desc;
    desc.input_path = "";
    desc.output_path = "/tmp/fuse_b79_cache_empty_input.fusemesh";
    expectTrue(fuse::project::hash_mesh_import(desc) == 0, "empty input path yields zero content hash");

    desc.input_path = "/tmp/fuse_b79_cache_empty_output.obj";
    desc.output_path = "";
    expectTrue(fuse::project::hash_mesh_import(desc) == 0, "empty output path yields zero content hash");
}

void testCookCacheEmptyGuards() {
    fuse::project::CookCache cache;
    expectTrue(cache.empty(), "fresh cache is empty");
    expectTrue(!cache.has_prunable_entries(), "empty cache has no prunable entries");

    expectTrue(cache.prune_stale_entries() == 0u, "prune_stale on empty cache returns zero");
    expectTrue(cache.prune_invalid_entries() == 0u, "prune_invalid on empty cache returns zero");
    expectTrue(cache.prune_all() == 0u, "prune_all on empty cache returns zero");

    expectTrue(!cache.invalidate(42u), "hash invalidation on empty cache is a no-op");
    expectTrue(!cache.contains(42u), "contains on empty cache rejects unknown hash");
    expectTrue(cache.stats().invalidations == 0u, "empty-cache hash invalidation does not bump stats");

    expectTrue(cache.invalidate_source("") == 0u, "empty source path invalidation is a no-op");
    expectTrue(cache.invalidate_output("") == 0u, "empty output path invalidation is a no-op");
    expectTrue(cache.invalidate_stale_content_for_source("", 42u) == 0u,
               "stale-content invalidation rejects empty source path");
    expectTrue(cache.invalidate_stale_content_for_source("/tmp/fuse_b79_missing.obj", 0u) == 0u,
               "stale-content invalidation rejects zero content hash");

    cache.invalidate_all();
    expectTrue(cache.stats().invalidations == 0u, "invalidate_all on empty cache does not bump stats");

    const fuse::u64 hits_before_clear = cache.stats().hits;
    cache.clear();
    expectTrue(cache.stats().hits == hits_before_clear, "clear on empty cache preserves stats");

    expectTrue(!cache.save(""), "save rejects empty path");
    expectTrue(!cache.load(""), "load rejects empty path");
    expectTrue(!cache.load("/tmp/fuse_b79_missing_cache.json"), "load rejects missing cache file");
}

void testCookCacheZeroKeyGuards() {
    fuse::project::CookCache cache;

    expectTrue(cache.lookup(0) == fuse::project::CookCacheLookup::Miss, "zero hash always misses");
    expectTrue(cache.stats().hits == 0u && cache.stats().misses == 0u,
               "zero-hash lookup does not touch hit/miss stats");

    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_zero_key.obj";
    invalid.output_path = "/tmp/fuse_b79_zero_key.fusemesh";
    cache.store(invalid);
    expectTrue(cache.entry_count() == 0u, "zero-hash entry is not stored");

    expectTrue(!cache.invalidate(0), "zero-hash invalidation is a no-op");
    expectTrue(!cache.contains(0), "contains rejects zero hash");
}

void testCookCachePruneAll() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 101;
    valid.source_path = "/tmp/fuse_b79_prune_all_valid.obj";
    valid.output_path = "/tmp/fuse_b79_prune_all_valid.fusemesh";
    cache.store(valid);

    fuse::project::CookCacheEntry invalid = valid;
    invalid.content_hash = 0;
    cache.store(invalid);
    expectTrue(cache.entry_count() == 1u, "store rejects invalid entry during prune_all setup");

    const std::string source = writeTempFile("/tmp/fuse_b79_prune_all_stale.obj", "# prune all v1\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_prune_all_stale.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "seed cook for prune_all ok");
    expectTrue(cooker.cache().entry_count() == 1u, "one stale-tracked entry before content change");

    writeTempFile(source, "# prune all v2\n");
    expectTrue(cooker.cache().lookup(first.content_hash) == fuse::project::CookCacheLookup::Hit,
               "stale entry still present before prune_all");

    const fuse::u32 removed = cooker.cache().prune_all();
    expectTrue(removed == 1u, "prune_all removes stale entry");
    expectTrue(cooker.cache().entry_count() == 0u, "cache empty after prune_all");
    expectTrue(cooker.cache().prune_all() == 0u, "second prune_all on empty cache is a no-op");
}

void testCookCacheStaleContentInvalidationGuards() {
    const std::string source = writeTempFile("/tmp/fuse_b79_stale_guard.obj", "# stale guard\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_stale_guard.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for stale-content guard ok");
    expectTrue(cooker.cache().entry_count() == 1u, "cache seeded");

    expectTrue(cooker.cache().invalidate_stale_content_for_source(source, 0u) == 0u,
               "zero current hash does not invalidate seeded entry");
    expectTrue(cooker.cache().entry_count() == 1u, "seeded entry survives zero-hash stale guard");

    expectTrue(cooker.cache().invalidate_stale_content_for_source(source, seeded.content_hash + 1u) == 1u,
               "mismatched current hash invalidates stale entry");
    expectTrue(cooker.cache().entry_count() == 0u, "cache empty after stale-content invalidation");
}

void testCookCacheHasPrunableEntriesAndCleanPruneGuards() {
    const std::string valid_source = writeTempFile("/tmp/fuse_b79_prunable_valid.obj", "# prunable valid\n");
    fuse::project::MeshImportDesc valid_desc;
    valid_desc.input_path = valid_source;
    valid_desc.output_path = "/tmp/fuse_b79_prunable_valid.fusemesh";

    fuse::project::AssetCooker fresh_cooker;
    const fuse::project::CookRecord valid_cook = fresh_cooker.cook_mesh(valid_desc);
    expectTrue(valid_cook.ok, "valid cook for prunable guard ok");
    expectTrue(!fresh_cooker.cache().has_prunable_entries(), "fresh cook cache is not prunable");
    expectTrue(fresh_cooker.cache().prune_all() == 0u, "prune_all on fresh cook cache is a no-op");

    const std::string source = writeTempFile("/tmp/fuse_b79_prunable_stale.obj", "# prunable v1\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_prunable_stale.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "seed cook for prunable guard ok");
    expectTrue(!cooker.cache().has_prunable_entries(), "fresh cook entry is not prunable");

    writeTempFile(source, "# prunable v2\n");
    expectTrue(cooker.cache().has_prunable_entries(), "stale content marks cache prunable");
    expectTrue(cooker.cache().prune_stale_entries() == 1u, "stale prune removes prunable entry");
    expectTrue(!cooker.cache().has_prunable_entries(), "cache is clean after stale prune");
}

void testCookCacheInvalidateUnknownHashGuards() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 401;
    valid.source_path = "/tmp/fuse_b79_unknown_inv.obj";
    valid.output_path = "/tmp/fuse_b79_unknown_inv.fusemesh";
    cache.store(valid);
    expectTrue(cache.entry_count() == 1u, "valid entry stored");

    const fuse::u64 invalidations_before = cache.stats().invalidations;
    expectTrue(!cache.invalidate(402u), "unknown hash invalidation is a no-op");
    expectTrue(cache.stats().invalidations == invalidations_before,
               "unknown hash invalidation does not bump stats");
    expectTrue(cache.entry_count() == 1u, "valid entry survives unknown hash invalidation");

    expectTrue(cache.invalidate(401u), "known hash invalidation removes entry");
    expectTrue(cache.entry_count() == 0u, "cache empty after known hash invalidation");
}

void testCookCacheLoadMissingFilePreservesEntries() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 501;
    valid.source_path = "/tmp/fuse_b79_preserve_load.obj";
    valid.output_path = "/tmp/fuse_b79_preserve_load.fusemesh";
    cache.store(valid);
    expectTrue(cache.entry_count() == 1u, "entry seeded before failed load");

    expectTrue(!cache.load("/tmp/fuse_b79_missing_preserve_cache.json"), "missing cache file load fails");
    expectTrue(cache.entry_count() == 1u, "failed load preserves existing entries");
    expectTrue(cache.contains(501u), "seeded entry remains after failed load");
}

void testCookCachePruneAllMixedInvalidAndStale() {
    const std::string valid_source = writeTempFile("/tmp/fuse_b79_prune_mixed_valid.obj", "# mixed valid\n");
    const std::string stale_source = writeTempFile("/tmp/fuse_b79_prune_mixed_stale.obj", "# mixed prune v1\n");

    fuse::project::MeshImportDesc valid_desc;
    valid_desc.input_path = valid_source;
    valid_desc.output_path = "/tmp/fuse_b79_prune_mixed_valid.fusemesh";

    fuse::project::MeshImportDesc stale_desc;
    stale_desc.input_path = stale_source;
    stale_desc.output_path = "/tmp/fuse_b79_prune_mixed_stale.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord valid_cook = cooker.cook_mesh(valid_desc);
    const fuse::project::CookRecord stale_cook = cooker.cook_mesh(stale_desc);
    expectTrue(valid_cook.ok && stale_cook.ok, "seed valid and stale entries for mixed prune");
    expectTrue(cooker.cache().entry_count() == 2u, "mixed cache has two fresh entries");

    writeTempFile(stale_source, "# mixed prune v2\n");
    expectTrue(cooker.cache().has_prunable_entries(), "stale entry makes mixed cache prunable");

    const fuse::u32 removed = cooker.cache().prune_all();
    expectTrue(removed == 1u, "prune_all removes only stale entry from mixed cache");
    expectTrue(cooker.cache().entry_count() == 1u, "valid entry survives mixed prune_all");
    expectTrue(cooker.cache().contains(valid_cook.content_hash), "non-stale entry remains addressable");
    expectTrue(!cooker.cache().has_prunable_entries(), "mixed cache is clean after prune_all");
}

void testFnv1a64BytesEmptyGuard() {
    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 0) == 14695981039346656037ull,
               "zero-size byte hash returns FNV offset basis");
}

void testHashUpstreamDependenciesEmptyPathGuards() {
    fuse::project::CookManifest manifest;

    expectTrue(fuse::project::hash_upstream_dependencies({}, manifest) == 0,
               "empty dependency list yields zero upstream hash");
    expectTrue(fuse::project::hash_upstream_dependencies({""}, manifest) == 0,
               "all-empty dependency paths yield zero upstream hash");
    expectTrue(fuse::project::hash_upstream_dependencies({"", ""}, manifest) == 0,
               "multiple empty dependency paths yield zero upstream hash");

    const std::string dep = writeTempFile("/tmp/fuse_b79_upstream_dep.obj", "# upstream dep\n");
    fuse::project::CookManifestEntry asset;
    asset.kind = fuse::project::CookAssetKind::Mesh;
    asset.source_path = dep;
    asset.output_path = "/tmp/fuse_b79_upstream_dep.fusemesh";
    manifest.assets.push_back(asset);

    const fuse::u64 with_empty =
        fuse::project::hash_upstream_dependencies({"", asset.output_path, ""}, manifest);
    const fuse::u64 without_empty = fuse::project::hash_upstream_dependencies({asset.output_path}, manifest);
    expectTrue(with_empty == without_empty, "empty dependency paths are skipped when folding upstream hash");
    expectTrue(without_empty != 0, "non-empty dependency path yields non-zero upstream hash");
}

void testHashManifestEntryEmptyDependencyGuards() {
    const std::string source = writeTempFile("/tmp/fuse_b79_manifest_dep.obj", "# manifest dep\n");

    fuse::project::CookManifestEntry with_empty_dep;
    with_empty_dep.kind = fuse::project::CookAssetKind::Mesh;
    with_empty_dep.source_path = source;
    with_empty_dep.output_path = "/tmp/fuse_b79_manifest_dep.fusemesh";
    with_empty_dep.dependencies = {"", ""};

    fuse::project::CookManifestEntry without_dep = with_empty_dep;
    without_dep.dependencies.clear();

    expectTrue(fuse::project::hash_manifest_entry(with_empty_dep) ==
                   fuse::project::hash_manifest_entry(without_dep),
               "empty manifest dependencies are skipped when hashing");
}

void testCookCacheLookupOutEntryGuard() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry sentinel;
    sentinel.content_hash = 999;
    sentinel.source_path = "/tmp/fuse_b79_lookup_sentinel.obj";
    sentinel.output_path = "/tmp/fuse_b79_lookup_sentinel.fusemesh";

    fuse::project::CookCacheEntry out = sentinel;
    expectTrue(cache.lookup(0, &out) == fuse::project::CookCacheLookup::Miss,
               "zero-hash lookup misses on empty cache");
    expectTrue(out.content_hash == sentinel.content_hash, "zero-hash lookup does not write out_entry");
    expectTrue(out.source_path == sentinel.source_path, "zero-hash lookup preserves out_entry source path");
}

void testCookCacheLookupEmptyCacheMissCounts() {
    fuse::project::CookCache cache;
    const fuse::u64 misses_before = cache.stats().misses;

    expectTrue(cache.lookup(77u) == fuse::project::CookCacheLookup::Miss,
               "valid hash misses on empty cache");
    expectTrue(cache.stats().misses == misses_before + 1u, "empty-cache lookup records one miss");
}

void testCookCacheShaderKindStalePrune() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry shader_entry;
    shader_entry.content_hash = 303;
    shader_entry.source_path = "/tmp/fuse_b79_shader_prune.obj";
    shader_entry.output_path = "/tmp/fuse_b79_shader_prune.fuseshader";
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    cache.store(shader_entry);
    expectTrue(cache.entry_count() == 1u, "shader entry stored with valid paths and hash");

    expectTrue(cache.has_prunable_entries(), "shader entry is prunable because recompute yields zero key");
    expectTrue(cache.prune_stale_entries() == 1u, "shader entry pruned as stale");
    expectTrue(cache.empty(), "cache empty after shader stale prune");
}

void testCookCacheLoadPrunesStaleEntries() {
    const std::string source = writeTempFile("/tmp/fuse_b79_load_prune.obj", "# load prune v1\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_load_prune.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord cooked = cooker.cook_mesh(desc);
    expectTrue(cooked.ok, "seed cook for load-time stale prune ok");

    const std::string cachePath = "/tmp/fuse_b79_load_prune_cache.json";
    expectTrue(cooker.cache().save(cachePath), "cache with fresh entry saves");

    writeTempFile(source, "# load prune v2\n");

    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "stale cache JSON loads");
    expectTrue(loaded.empty(), "load prunes stale entries after content change");
    expectTrue(loaded.stats().invalidations >= 1u, "load-time stale prune counts as invalidation");
}

void testCookCacheEmptyPathTextureAudioGuards() {
    fuse::project::TextureImportDesc tex;
    tex.input_path = "";
    tex.output_path = "/tmp/fuse_b79_tex_empty.fusetex";
    expectTrue(fuse::project::hash_texture_import(tex) == 0, "empty texture input path yields zero hash");

    fuse::project::AudioImportDesc audio;
    audio.input_path = "/tmp/fuse_b79_audio_empty.wav";
    audio.output_path = "";
    expectTrue(fuse::project::hash_audio_import(audio) == 0, "empty audio output path yields zero hash");
}

void testCookCacheLoadPreservesEntriesWithoutOnDiskSource() {
    const std::string cachePath = "/tmp/fuse_b79_missing_source_load.json";
    {
        std::ofstream out(cachePath, std::ios::binary);
        out << R"({
  "schemaVersion": 1,
  "entries": [
    {
      "contentHash": 101,
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_valid.fusemesh",
      "sourcePath": "/tmp/fuse_b79_valid.obj",
      "kind": "mesh"
    },
    {
      "contentHash": 0,
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_zero_hash.fusemesh",
      "sourcePath": "/tmp/fuse_b79_zero_hash.obj",
      "kind": "mesh"
    }
  ]
}
)";
    }

    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "cache with missing on-disk source loads");
    expectTrue(loaded.entry_count() == 1u,
               "load keeps valid entry when source file is absent during stale reconcile");
    expectTrue(loaded.contains(101u), "absent-source entry remains addressable after load");
}

void testCookHashPreflightGuards() {
    expectTrue(!fuse::project::is_valid_fnv1a64_input(nullptr, 4u),
               "null data with non-zero size fails FNV input validation");
    expectTrue(fuse::project::is_valid_fnv1a64_input(nullptr, 0),
               "null data with zero size passes FNV input validation");

    const fuse::project::CookHashPreflight empty_path =
        fuse::project::preflight_file_content_hash("");
    expectTrue(!empty_path.ok(), "empty path fails file hash preflight");
    expectTrue(empty_path.reason == fuse::project::CookHashRejectReason::EmptyPath,
               "empty path preflight reason is EmptyPath");

    expectTrue(!fuse::project::preflight_file_content_hash("/tmp/fuse_b79_missing_preflight.obj").ok(),
               "missing file fails file hash preflight");
    expectTrue(fuse::project::preflight_file_content_hash("/tmp/fuse_b79_missing_preflight.obj").reason ==
                   fuse::project::CookHashRejectReason::SourceUnreadable,
               "missing file preflight reason is SourceUnreadable");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_mesh.obj", "# preflight mesh\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_preflight_mesh.fusemesh";
    const fuse::project::CookHashPreflight mesh_preflight = fuse::project::preflight_mesh_import_hash(desc);
    expectTrue(mesh_preflight.ok(), "readable mesh import passes hash preflight");
    expectTrue(fuse::project::hash_mesh_import(desc) != 0, "preflight success implies non-zero mesh hash");

    desc.input_path = "";
    expectTrue(fuse::project::preflight_mesh_import_hash(desc).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty mesh input path preflight reason");

    const fuse::project::CookHashPreflight zero_key = fuse::project::preflight_cook_cache_key(0, 42u);
    expectTrue(!zero_key.ok(), "zero source hash fails cache key preflight");
    expectTrue(zero_key.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero source hash preflight reason");

    const fuse::project::CookHashPreflight valid_key = fuse::project::preflight_cook_cache_key(99u, 42u);
    expectTrue(valid_key.ok(), "non-zero source hash passes cache key preflight");

    fuse::project::CookManifest manifest;
    expectTrue(!fuse::project::preflight_upstream_dependencies_hash({}, manifest).ok(),
               "empty dependency list fails upstream hash preflight");
    expectTrue(fuse::project::preflight_upstream_dependencies_hash({}, manifest).reason ==
                   fuse::project::CookHashRejectReason::EmptyDependencyList,
               "empty dependency list preflight reason");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::EmptyPath)) == "empty_path",
               "reject reason label for empty path");
}

void testCookHashPreflightShouldSkip() {
    const fuse::project::CookHashPreflight empty_path =
        fuse::project::preflight_file_content_hash("");
    expectTrue(empty_path.should_skip(), "empty path preflight should_skip");
    expectTrue(empty_path.should_skip() == !empty_path.ok(), "should_skip mirrors !ok");

    const std::string source = writeTempFile("/tmp/fuse_b79_should_skip_mesh.obj", "# should skip mesh\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_should_skip_mesh.fusemesh";
    const fuse::project::CookHashPreflight mesh_preflight = fuse::project::preflight_mesh_import_hash(desc);
    expectTrue(!mesh_preflight.should_skip(), "readable mesh preflight should not skip");

    const fuse::project::CookHashPreflight null_bytes =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(null_bytes.should_skip(), "null bytes preflight should_skip");
}

void testCookCachePruneEstimateShouldSkip() {
    fuse::project::CookCache cache;
    const fuse::project::CookCachePruneEstimate empty = cache.estimate_prune_removals();
    expectTrue(empty.should_skip(), "empty cache prune estimate should_skip");
    expectTrue(empty.should_skip() == (empty.total() == 0u), "should_skip mirrors zero total");
    expectTrue(!cache.would_prune_all(), "would_prune_all is false when estimate should_skip");

    fuse::project::CookCacheEntry shader_entry;
    shader_entry.content_hash = 909;
    shader_entry.source_path = "/tmp/fuse_b79_should_skip_shader.obj";
    shader_entry.output_path = "/tmp/fuse_b79_should_skip_shader.fuseshader";
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    cache.store(shader_entry);

    const fuse::project::CookCachePruneEstimate stale = cache.estimate_prune_removals();
    expectTrue(!stale.should_skip(), "stale shader prune estimate should not skip");
    expectTrue(cache.would_prune_all(), "would_prune_all true when estimate should not skip");
}

void testCookCacheInvalidationProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate(42u), "would_invalidate on empty cache is false");
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_probe.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_probe.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_probe.obj", 1u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_probe.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_probe.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");
    expectTrue(cache.count_by_source("/tmp/fuse_b79_probe.obj") == 0u,
               "count_by_source on empty cache returns zero");
    expectTrue(cache.count_prunable_entries() == 0u, "count_prunable on empty cache returns zero");
    expectTrue(cache.count_invalid_entries() == 0u, "count_invalid on empty cache returns zero");
    expectTrue(cache.probe_stale_upstream_sources({{"/tmp/fuse_b79_probe.obj", 1u}}).empty(),
               "probe_stale_upstream on empty cache returns empty list");

    const std::string source = writeTempFile("/tmp/fuse_b79_probe_mesh.obj", "# probe mesh\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_probe_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for invalidation probes ok");

    expectTrue(cooker.cache().would_invalidate(seeded.content_hash),
               "would_invalidate reports seeded hash");
    expectTrue(!cooker.cache().would_invalidate(0), "would_invalidate rejects zero hash");
    expectTrue(!cooker.cache().would_invalidate(seeded.content_hash + 1u),
               "would_invalidate rejects unknown hash");
    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source finds seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output finds seeded entry");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output rejects empty path");
    expectTrue(cooker.cache().count_by_source(source) == 1u, "count_by_source finds seeded entry");
    expectTrue(cooker.cache().count_by_output(desc.output_path) == 1u, "count_by_output finds seeded entry");
    expectTrue(cooker.cache().count_stale_content_for_source(source, seeded.content_hash) == 0u,
               "count_stale_content with matching hash returns zero");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false with matching hash");
    expectTrue(cooker.cache().count_stale_content_for_source(source, seeded.content_hash + 1u) == 1u,
               "count_stale_content with mismatched hash returns one");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true with mismatched hash");

    writeTempFile(source, "# probe mesh updated\n");
    expectTrue(cooker.cache().count_prunable_entries() == 1u, "count_prunable reports stale entry");
    expectTrue(cooker.cache().count_invalid_entries() == 0u,
               "count_invalid on structurally valid stale entry returns zero");

    const fuse::u32 removed = cooker.cache().prune_stale_entries();
    expectTrue(removed == 1u, "prune removes probed stale entry");
    expectTrue(cooker.cache().count_prunable_entries() == 0u, "count_prunable zero after prune");
}

void testCookCachePruneReconcileEstimateGuards() {
    fuse::project::CookCache cache;
    const fuse::project::CookCachePruneEstimate empty = cache.estimate_prune_removals();
    expectTrue(empty.total() == 0u, "empty cache prune estimate is zero");
    expectTrue(!cache.would_prune_all(), "empty cache would_prune_all is false");
    expectTrue(cache.count_stale_entries() == 0u, "empty cache stale count is zero");
    expectTrue(cache.probe_stale_content_sources().empty(), "empty cache stale source probe is empty");

    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_est_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_est_invalid.fusemesh";
    cache.store(invalid);
    expectTrue(cache.entry_count() == 0u, "invalid entry rejected during estimate setup");

    fuse::project::CookCacheEntry shader_entry;
    shader_entry.content_hash = 808;
    shader_entry.source_path = "/tmp/fuse_b79_est_shader.obj";
    shader_entry.output_path = "/tmp/fuse_b79_est_shader.fuseshader";
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    cache.store(shader_entry);
    expectTrue(cache.entry_count() == 1u, "shader entry stored for prune estimate");

    const fuse::project::CookCachePruneEstimate estimate = cache.estimate_prune_removals();
    expectTrue(estimate.stale_entries == 1u, "shader entry counted as stale in estimate");
    expectTrue(estimate.invalid_entries == 0u, "shader entry is structurally valid");
    expectTrue(cache.would_prune_all(), "shader stale entry makes would_prune_all true");
    expectTrue(cache.count_stale_entries() == 1u, "count_stale_entries matches estimate");
    expectTrue(cache.count_prunable_entries() == estimate.total(), "count_prunable matches estimate total");

    const fuse::u32 removed = cache.prune_all();
    expectTrue(removed == estimate.total(), "prune_all removes estimated total");
    expectTrue(cache.estimate_prune_removals().total() == 0u, "estimate zero after prune_all");
}

void testCookCacheProbeStaleContentSources() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_probe_stale_a.obj", "# probe stale a v1\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_probe_stale_b.obj", "# probe stale b v1\n");

    fuse::project::MeshImportDesc desc_a;
    desc_a.input_path = source_a;
    desc_a.output_path = "/tmp/fuse_b79_probe_stale_a.fusemesh";

    fuse::project::MeshImportDesc desc_b;
    desc_b.input_path = source_b;
    desc_b.output_path = "/tmp/fuse_b79_probe_stale_b.fusemesh";

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_mesh(desc_a).ok, "seed cook a for stale source probe");
    expectTrue(cooker.cook_mesh(desc_b).ok, "seed cook b for stale source probe");
    expectTrue(cooker.cache().probe_stale_content_sources().empty(), "fresh entries not probed as stale");

    writeTempFile(source_a, "# probe stale a v2\n");
    writeTempFile(source_b, "# probe stale b v2\n");

    const std::vector<std::string> stale_sources = cooker.cache().probe_stale_content_sources();
    expectTrue(stale_sources.size() == 2u, "two unique stale sources probed");
    expectTrue(stale_sources[0] == source_a || stale_sources[1] == source_a, "source a in stale probe");
    expectTrue(stale_sources[0] == source_b || stale_sources[1] == source_b, "source b in stale probe");
}

void testCookHashPreflightFnvAndCombineGuards() {
    const fuse::project::CookHashPreflight null_bytes =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_bytes.ok(), "null bytes with non-zero size fails preflight");
    expectTrue(null_bytes.reason == fuse::project::CookHashRejectReason::NullData,
               "null bytes preflight reason is NullData");

    const fuse::project::CookHashPreflight empty_bytes =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 0);
    expectTrue(empty_bytes.ok(), "zero-size null bytes passes preflight");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_tex.png", "PNG\n");
    fuse::project::TextureImportDesc tex;
    tex.input_path = source;
    tex.output_path = "/tmp/fuse_b79_preflight_tex.fusetex";
    expectTrue(fuse::project::preflight_texture_import_hash(tex).ok(), "texture import preflight ok");

    fuse::project::AudioImportDesc audio;
    audio.input_path = source;
    audio.output_path = "/tmp/fuse_b79_preflight_audio.fuseaudio";
    expectTrue(fuse::project::preflight_audio_import_hash(audio).ok(), "audio import preflight ok");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_preflight_manifest.fusemesh";
    expectTrue(fuse::project::preflight_manifest_entry_hash(entry).ok(), "manifest entry preflight ok");

    expectTrue(!fuse::project::preflight_combine_cook_cache_key(0, 42u).ok(),
               "combine cache key preflight rejects zero source");
    expectTrue(fuse::project::preflight_combine_cook_cache_key(99u, 0).ok(),
               "combine cache key preflight allows zero upstream");
    expectTrue(fuse::project::preflight_combine_cook_cache_key(99u, 42u).ok(),
               "combine cache key preflight ok for valid fold");
}

void testCookCacheWouldInvalidationProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_down.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would mesh\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content with matching hash is false");

    writeTempFile(source, "# would mesh updated\n");
    const fuse::u64 updated_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(updated_hash != seeded.content_hash, "content change yields new hash key");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, updated_hash),
               "would_invalidate_stale_content with updated hash is true");
    expectTrue(cooker.cache().count_stale_content_for_source(source, updated_hash) == 1u,
               "count_stale_content matches would_invalidate_stale_content");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 909;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");

    fuse::project::CookCacheEntry zero_hash = valid;
    zero_hash.content_hash = 0;
    const fuse::project::CookHashPreflight zero_preflight =
        fuse::project::preflight_cook_cache_entry(zero_hash);
    expectTrue(!zero_preflight.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_preflight.reason == fuse::project::CookHashRejectReason::ZeroContentHash,
               "zero content hash preflight reason");

    fuse::project::CookCacheEntry empty_source = valid;
    empty_source.source_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path preflight reason");

    fuse::project::CookCacheEntry empty_output = valid;
    empty_output.output_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_output).reason ==
                   fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output path preflight reason");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::ZeroContentHash)) == "zero_content_hash",
               "reject reason label for zero content hash");
}

void testCookCachePruneInvalidEntriesOnLoad() {
    const std::string source = writeTempFile("/tmp/fuse_b79_prune_load_valid.obj", "# prune load valid\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_prune_load_valid.fusemesh";
    const fuse::u64 valid_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(valid_hash != 0, "valid source produces non-zero content hash for load test");

    const std::string cachePath = "/tmp/fuse_b79_prune_invalid_load.json";
    {
        std::ofstream out(cachePath, std::ios::binary);
        out << "{\n  \"schemaVersion\": 1,\n  \"entries\": [\n    {\n";
        out << "      \"contentHash\": " << valid_hash << ",\n";
        out << R"(
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_prune_load_valid.fusemesh",
      "sourcePath": "/tmp/fuse_b79_prune_load_valid.obj",
      "kind": "mesh"
    },
    {
      "contentHash": 0,
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_prune_load_zero.fusemesh",
      "sourcePath": "/tmp/fuse_b79_prune_load_zero.obj",
      "kind": "mesh"
    }
  ]
}
)";
    }

    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "mixed cache JSON loads");
    expectTrue(loaded.entry_count() == 1u, "load rejects invalid trailing records");
    expectTrue(loaded.prune_invalid_entries() == 0u, "prune_invalid on clean load is a no-op");
    expectTrue(loaded.contains(valid_hash), "valid loaded entry remains addressable");
}

} // namespace

int main() {
    fuse::core::initialize();

    testCombineCookCacheKeyGuards();
    testFnv1a64BytesEmptyGuard();
    testHashUpstreamDependenciesEmptyPathGuards();
    testHashManifestEntryEmptyDependencyGuards();
    testCookCacheEmptyPathRejection();
    testCookCacheEmptyPathTextureAudioGuards();
    testCookCacheEmptyGuards();
    testCookCacheZeroKeyGuards();
    testCookCacheLookupOutEntryGuard();
    testCookCacheLookupEmptyCacheMissCounts();
    testCookCachePruneAll();
    testCookCacheStaleContentInvalidationGuards();
    testCookCacheHasPrunableEntriesAndCleanPruneGuards();
    testCookCacheInvalidateUnknownHashGuards();
    testCookCacheLoadMissingFilePreservesEntries();
    testCookCacheShaderKindStalePrune();
    testCookCacheLoadPrunesStaleEntries();
    testCookCacheLoadPreservesEntriesWithoutOnDiskSource();
    testCookCachePruneAllMixedInvalidAndStale();
    testCookHashPreflightGuards();
    testCookHashPreflightFnvAndCombineGuards();
    testCookHashPreflightShouldSkip();
    testCookCachePruneEstimateShouldSkip();
    testCookCacheInvalidationProbes();
    testCookCachePruneReconcileEstimateGuards();
    testCookCacheProbeStaleContentSources();
    testCookCacheWouldInvalidationProbes();
    testCookCacheEntryPreflightGuards();
    testCookCachePruneInvalidEntriesOnLoad();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// --- deepen additive from deepen-b79-cooker-hash-prune-guards-0c0b ---
void testCookContentHashNullAndReadableGuards() {
void testCookCacheInvalidAndStaleEntryHelpers() {
void testCookCacheHasInvalidAndStaleEntryGuards() {
    testCookCacheInvalidAndStaleEntryHelpers();
    testCookCacheHasInvalidAndStaleEntryGuards();

// --- deepen additive from b79-cooker-hash-guards-111f ---
void testCookCacheLookupPreflightGuards() {
    expectTrue(zero_preflight.should_skip(), "zero-key lookup preflight should skip");
void testCookCacheStorePreflightGuards() {
    expectTrue(zero_preflight.should_skip(), "zero-key store preflight should skip");
    expectTrue(empty_source_preflight.should_skip(), "empty source store preflight should skip");
void testCookImportHashPreflightGuards() {
    expectTrue(unreadable.should_skip(), "unreadable source import preflight should skip");
    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_probe_a.fusemesh";
    manifest.assets.push_back(entryA);
    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_probe_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);
    expectTrue(upstream_probe.would_invalidate(), "upstream probe reports invalidation scope");
    testCookCacheLookupPreflightGuards();
    testCookCacheStorePreflightGuards();
    testCookImportHashPreflightGuards();

// --- deepen additive from deepen-b79-cooker-hash-guards-7fd3 ---
void testCookCacheHashPreflightGuards() {
    const fuse::project::CookCacheKeyPreflight zero_source =
    expectTrue(zero_source.reason == fuse::project::CookCacheKeyRejectReason::ZeroSourceHash,
    const fuse::project::CookCacheKeyPreflight valid_source =
    const fuse::project::CookFileHashPreflight empty_path =
    expectTrue(empty_path.reason == fuse::project::CookFileHashRejectReason::EmptyPath,
    const fuse::project::CookFileHashPreflight missing =
    expectTrue(missing.reason == fuse::project::CookFileHashRejectReason::UnreadableSource,
    const fuse::project::CookFileHashPreflight readable =
    const fuse::project::CookCacheKeyPreflight entry_preflight =
    expectTrue(!empty.probe_invalidate_source("/tmp/fuse_b79_probe.obj").would_invalidate(),
    expectTrue(stale_probe.would_invalidate(), "stale-content probe detects mismatched hash");
    testCookCacheHashPreflightGuards();

// --- deepen additive from b79-hash-preflight-probes-fd33 ---
    fuse::project::CookHashPreflightRejectReason reason = fuse::project::CookHashPreflightRejectReason::None;
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::EmptyPath,
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::MissingFile,
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::None,
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::EmptyInputOrOutput,
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::ZeroSourceHash,
    expectTrue(cooker.cache().probe_would_invalidate_hash(seeded.content_hash),
    expectTrue(!cooker.cache().probe_would_invalidate_hash(seeded.content_hash + 1u),
    expectTrue(cooker.cache().probe_would_invalidate_source(source),
    expectTrue(!cooker.cache().probe_would_invalidate_source(""),

// --- deepen additive from deepen-b79-cooker-hash-preflight-27fe ---
void testContentHashPreflightGuards() {
    const fuse::project::CookContentHashPreflight empty_path =
    const fuse::project::CookContentHashPreflight missing =
    const fuse::project::CookContentHashPreflight ok = fuse::project::preflight_file_content_hash(source);
    const fuse::project::CookImportHashPreflight mesh_preflight = fuse::project::preflight_mesh_import(mesh);
    const fuse::project::CookImportHashPreflight mesh_ok = fuse::project::preflight_mesh_import(mesh);
void testCookCachePreflightAndLookupGuards() {
    const fuse::project::CookCacheStorePreflight store_preflight =
    const fuse::project::CookCacheLookupPreflight empty_lookup = cache.preflight_lookup(77u);
    const fuse::project::CookCacheLookupPreflight hit_lookup = fuse::project::preflight_cook_cache_lookup(cache, 808u);
    expectTrue(!unknown_hash.would_invalidate(), "unknown hash probe reports no removal");
    expectTrue(known_hash.would_invalidate(), "known hash probe reports removal");
    expectTrue(known_hash.would_invalidate_count == 1u, "known hash probe counts one entry");
    expectTrue(source_probe.would_invalidate_count == 1u, "source probe counts seeded entry");
    expectTrue(stale_probe.would_invalidate_count == 1u, "stale-content probe counts mismatched entry");
    expectTrue(output_probe.would_invalidate_count == 1u, "output probe counts seeded entry");
    expectTrue(!cache.probe_invalidate_downstream_of("", {}, {}).would_invalidate(),
void testCookCacheReconcileEstimatorGuards() {
void testCookCacheStaleClassificationGuards() {
    testContentHashPreflightGuards();
    testCookCachePreflightAndLookupGuards();

// --- deepen additive from deepen-b79-cooker-hash-7359 ---
void testCookCacheKeyPreflight() {
    fuse::project::CookCacheKeyRejectReason reason = fuse::project::CookCacheKeyRejectReason::None;
    expectTrue(reason == fuse::project::CookCacheKeyRejectReason::ZeroSource,
    expectTrue(reason == fuse::project::CookCacheKeyRejectReason::None,
    expectTrue(std::string(fuse::project::cookCacheKeyRejectReasonLabel(
                   fuse::project::CookCacheKeyRejectReason::ZeroSource)) == "zero_source",
void testCookHashPreflight() {
    fuse::project::CookHashRejectReason reason = fuse::project::CookHashRejectReason::None;
    expectTrue(reason == fuse::project::CookHashRejectReason::EmptyPath,
    expectTrue(reason == fuse::project::CookHashRejectReason::Unreadable,
    expectTrue(reason == fuse::project::CookHashRejectReason::None,
void testCookCachePreflightLookupAndStore() {
    fuse::project::CookCache::LookupRejectReason lookup_reason =
        fuse::project::CookCache::LookupRejectReason::None;
    expectTrue(lookup_reason == fuse::project::CookCache::LookupRejectReason::ZeroKey,
    expectTrue(lookup_reason == fuse::project::CookCache::LookupRejectReason::EmptyCache,
    fuse::project::CookCache::StoreRejectReason store_reason =
        fuse::project::CookCache::StoreRejectReason::None;
    expectTrue(store_reason == fuse::project::CookCache::StoreRejectReason::None,
    expectTrue(store_reason == fuse::project::CookCache::StoreRejectReason::InvalidEntry,
    expectTrue(lookup_reason == fuse::project::CookCache::LookupRejectReason::None,
    expectTrue(lookup_reason == fuse::project::CookCache::LookupRejectReason::NotFound,
    testCookCacheKeyPreflight();
    testCookHashPreflight();
    testCookCachePreflightLookupAndStore();

// --- deepen additive from deepen-b79-cooker-hash-preflight-0c1f ---
                   fuse::project::CookHashPreflightReject::EmptyPath,
                   fuse::project::CookHashPreflightReject::MissingFile,
    const fuse::project::CookHashPreflight ok = fuse::project::preflight_hash_file_content(source);
    expectTrue(ok.reject == fuse::project::CookHashPreflightReject::None, "ok preflight reject is None");
                   fuse::project::CookHashPreflightReject::ZeroKey,

// --- deepen additive from deepen-b79-cooker-hash-preflight-633e ---
    const fuse::project::CookCacheKeyPreflight zero =
    const fuse::project::CookCacheKeyPreflight source_only =
    const fuse::project::CookCacheKeyPreflight combined =
void testCookCacheEntryPreflight() {
    const fuse::project::CookCacheEntryPreflight invalid_preflight =
    const fuse::project::CookCacheEntryPreflight fresh_preflight =
    const fuse::project::CookCacheEntryPreflight stale_preflight =
    testCookCacheEntryPreflight();

// --- deepen additive from b79-hash-preflight-probes-15d5 ---
    expectTrue(cooker.cache().probe_would_invalidate_output(desc.output_path),
    expectTrue(!cooker.cache().probe_would_invalidate_output(""),
    expectTrue(!cooker.cache().probe_would_invalidate_stale_content(source, seeded.content_hash),
    expectTrue(cooker.cache().probe_would_invalidate_stale_content(source, seeded.content_hash + 1u),

// --- deepen additive from deepen-b79-cooker-hash-preflight-e529 ---
void testCookContentHashPreflightGuards() {
    expectTrue(fuse::project::should_skip_hash_file_content(""), "empty path skips file hash preflight");
    const fuse::project::CookHashPreflight empty_preflight = fuse::project::preflight_hash_file_content("");
    expectTrue(empty_preflight.should_skip(), "empty path preflight should skip");
    expectTrue(fuse::project::should_skip_hash_file_content("/tmp/fuse_b79_preflight_missing.obj"),
    const fuse::project::CookHashPreflight missing_preflight =
    expectTrue(missing_preflight.should_skip(), "missing file preflight should skip");
    const fuse::project::CookHashPreflight valid_preflight =
    expectTrue(!valid_preflight.should_skip(), "valid file preflight does not skip");
    expectTrue(fuse::project::preflight_hash_mesh_import(desc).should_skip(),
    const fuse::project::CookCacheKeyPreflight key_preflight =
    expectTrue(fuse::project::should_skip_combine_cook_cache_key(0, 42u),
    expectTrue(empty_probe.should_skip(), "probe on empty cache should skip");
    expectTrue(zero_probe.should_skip(), "zero hash probe should skip");
    expectTrue(known_probe.would_invalidate(), "known hash probe would invalidate");
    expectTrue(unknown_probe.should_skip(), "unknown hash probe should skip");
    expectTrue(source_probe.would_invalidate(), "source probe would invalidate");
    expectTrue(output_probe.would_invalidate(), "output probe would invalidate");
    expectTrue(cache.probe_invalidate_stale_content_for_source(valid.source_path, 802u).would_invalidate(),
    expectTrue(cache.probe_invalidate_stale_content_for_source(valid.source_path, 801u).should_skip(),
    expectTrue(empty_estimate.should_skip(), "empty cache reconcile estimate should skip");
    expectTrue(cooker.cache().estimate_prune_all().should_skip(),
    testCookContentHashPreflightGuards();

// --- deepen additive from deepen-b79-cooker-hash-preflight-34cc ---
    const fuse::project::CookHashPreflight empty_path = fuse::project::preflight_hash_file_content("");
    const fuse::project::CookHashPreflight missing =
    const fuse::project::CookHashPreflight mesh_empty_input = fuse::project::preflight_mesh_import(mesh);
    const fuse::project::CookHashPreflight mesh_empty_output = fuse::project::preflight_mesh_import(mesh);

// --- deepen additive from deepen-b79-cooker-hash-0896 ---
void testCookHashPreflightFnvAndCacheEntryGuards() {
    const fuse::project::CookHashPreflight null_data = fuse::project::preflight_fnv1a64_input(nullptr, 4u);
    expectTrue(null_data.reason == fuse::project::CookHashRejectReason::NullData,
    const fuse::project::CookHashPreflight empty_data = fuse::project::preflight_fnv1a64_input(nullptr, 0);
    const fuse::project::CookHashPreflight zero_entry =
    expectTrue(zero_entry.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_src.obj", 42u),
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_src.obj", 1u}}),
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_out.fusemesh", {}, {}),
               "would_invalidate_stale_content with mismatched hash is true");
    testCookHashPreflightFnvAndCacheEntryGuards();

// --- deepen additive from deepen-b79-cooker-hash-guards-709d ---
    const fuse::project::CookHashPreflight null_fnv =
    expectTrue(null_fnv.reason == fuse::project::CookHashRejectReason::NullData,

// --- deepen additive from deepen-b79-cooker-hash-21b9 ---
void testCookHashPreflightFnv1a64Guard() {
    const fuse::project::CookHashPreflight valid_data =
               "would_invalidate_source reports seeded source");
               "would_invalidate_output reports seeded output");
               "would_invalidate_stale_content reports mismatched hash");
               "would_invalidate_stale_content rejects matching hash");
    testCookHashPreflightFnv1a64Guard();

// --- deepen additive from deepen-b79-cooker-hash-314f ---
    const fuse::project::CookHashPreflight null_fnv = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    const fuse::project::CookHashPreflight non_cacheable =
    expectTrue(non_cacheable.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
    const fuse::project::CookHashPreflight cacheable =
                   fuse::project::CookHashRejectReason::NonCacheableKey)) == "non_cacheable_key",

// --- deepen additive from deepen-b79-cooker-hash-reconcile-b4f0 ---
                   .reason == fuse::project::CookHashRejectReason::SourceUnreadable,
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_inc_probe.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_inc_probe.fusemesh"),
    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path probe is false");

// --- deepen additive from deepen-b79-cooker-hash-guards-2061 ---
    const fuse::project::CookHashPreflight combine_zero =
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_inv.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_inv.fusemesh"),
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_inv.obj", 42u),

// --- deepen additive from deepen-b79-cooker-hash-preflight-6a72 ---
void testCookCacheLookupStorePreflights() {
    const fuse::project::CookCacheLookupPreflight zero_lookup =
    expectTrue(zero_lookup.should_skip(), "zero-key lookup preflight skips");
    const fuse::project::CookCacheStorePreflight valid_store =
    expectTrue(fuse::project::preflight_cook_cache_store(entry).should_skip(),
void testCookFnvInputPreflight() {
    const fuse::project::CookFnvInputPreflight null_preflight =
    expectTrue(null_preflight.should_skip(), "null data with size fails FNV preflight");
    const fuse::project::CookFnvInputPreflight empty_preflight =
    testCookCacheLookupStorePreflights();
    testCookFnvInputPreflight();

// --- deepen additive from deepen-b79-cooker-hash-be66 ---
void testCookHashPreflightDeepenGuards() {
    const fuse::project::CookHashPreflight null_bytes = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    const fuse::project::CookHashPreflight zero_source =
    expectTrue(zero_source.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
                   fuse::project::CookHashRejectReason::NonCacheableCombinedKey)) == "non_cacheable_combined_key",
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_deepen_probe.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_deepen_probe.fusemesh"),
    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path would_invalidate is false");
    testCookHashPreflightDeepenGuards();

// --- deepen additive from deepen-b79-cooker-hash-13ca ---
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_source.obj", 42u),
    expectTrue(seeded.ok, "seed cook for would_invalidate mirrors ok");
               "would_invalidate_source mirrors count_by_source");
               "would_invalidate_output mirrors count_by_output");
               "matching hash would_invalidate_stale_content is false");
               "mismatched hash would_invalidate_stale_content is true");
