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
    testCookCachePruneInvalidEntriesOnLoad();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
