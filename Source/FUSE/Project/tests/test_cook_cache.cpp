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

void testCookCacheLookupEmptyCacheMissCounts() {
    const fuse::u64 misses_before = cache.stats().misses;

    expectTrue(cache.lookup(77u) == fuse::project::CookCacheLookup::Miss,
               "valid hash misses on empty cache");
    expectTrue(cache.stats().misses == misses_before + 1u, "empty-cache lookup records one miss");

void testCookCacheShaderKindStalePrune() {

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

void testCookCacheEmptyPathTextureAudioGuards() {
    fuse::project::TextureImportDesc tex;
    tex.input_path = "";
    tex.output_path = "/tmp/fuse_b79_tex_empty.fusetex";
    expectTrue(fuse::project::hash_texture_import(tex) == 0, "empty texture input path yields zero hash");

    fuse::project::AudioImportDesc audio;
    audio.input_path = "/tmp/fuse_b79_audio_empty.wav";
    audio.output_path = "";
    expectTrue(fuse::project::hash_audio_import(audio) == 0, "empty audio output path yields zero hash");

void testCookCacheLoadPreservesEntriesWithoutOnDiskSource() {
    const std::string cachePath = "/tmp/fuse_b79_missing_source_load.json";
void testCookContentHashByteSpanGuards() {
    expectTrue(!fuse::project::is_hashable_byte_span(nullptr, 0), "null zero-length span is not hashable");
    expectTrue(!fuse::project::is_hashable_byte_span(nullptr, 4u), "null non-zero span is not hashable");

    const fuse::u8 byte = 7;
    expectTrue(fuse::project::is_hashable_byte_span(&byte, 1u), "non-null span is hashable");
    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 4u) == 0, "null non-zero span hashes to zero");
    expectTrue(fuse::project::fnv1a64_bytes(&byte, 1u) != 0, "valid span yields non-zero hash");

void testCookCacheInvalidVsStalePruneGuards() {

    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_split_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_split_invalid.fusemesh";
    cache.store(invalid);
    expectTrue(cache.entry_count() == 0u, "store rejects invalid entry for split prune guard");

    const std::string source = writeTempFile("/tmp/fuse_b79_split_stale.obj", "# split stale v1\n");
    desc.output_path = "/tmp/fuse_b79_split_stale.fusemesh";

    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "seed cook for split prune guard ok");
    expectTrue(!cooker.cache().has_invalid_entries(), "fresh cook cache has no invalid entries");
    expectTrue(!cooker.cache().has_stale_entries(), "fresh cook cache has no stale entries");
    expectTrue(!cooker.cache().has_prunable_entries(), "fresh cook cache is not prunable");

    writeTempFile(source, "# split stale v2\n");
    expectTrue(!cooker.cache().has_invalid_entries(), "stale content does not mark invalid entries");
    expectTrue(cooker.cache().has_stale_entries(), "stale content marks stale entries");
    expectTrue(cooker.cache().has_prunable_entries(), "stale cache is prunable");
    expectTrue(cooker.cache().prune_invalid_entries() == 0u, "invalid prune no-op on stale-only cache");
    expectTrue(cooker.cache().prune_stale_entries() == 1u, "stale prune removes stale entry");
    expectTrue(!cooker.cache().has_stale_entries(), "cache is clean after stale prune");

void testCookCacheLoadCorruptPreservesEntries() {

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 601;
    valid.source_path = "/tmp/fuse_b79_corrupt_load.obj";
    valid.output_path = "/tmp/fuse_b79_corrupt_load.fusemesh";
    cache.store(valid);
    expectTrue(cache.entry_count() == 1u, "entry seeded before corrupt load");

    const std::string corruptPath = "/tmp/fuse_b79_corrupt_cache.json";
    writeTempFile(corruptPath, "{ not a cook cache document }\n");
    expectTrue(!cache.load(corruptPath), "corrupt cache JSON load fails");
    expectTrue(cache.entry_count() == 1u, "corrupt load preserves existing entries");
    expectTrue(cache.contains(601u), "seeded entry remains after corrupt load");
void testCookContentHashNullAndReadableGuards() {
    expectTrue(fuse::project::is_zero_cook_hash(0), "zero cook hash is reserved");
    expectTrue(!fuse::project::is_zero_cook_hash(42u), "non-zero hash is not zero");

    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 0) != 0, "null empty-byte hash is defined");
    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 4) == 0, "null non-empty byte hash is rejected");

    expectTrue(!fuse::project::is_readable_cook_source_path(""), "empty path is not readable");
    expectTrue(!fuse::project::is_readable_cook_source_path("/tmp/fuse_b79_missing_readable.obj"),
               "missing path is not readable");

    const std::string source = writeTempFile("/tmp/fuse_b79_readable.obj", "# readable source\n");
    expectTrue(fuse::project::is_readable_cook_source_path(source), "existing source path is readable");

void testCookCacheInvalidAndStaleEntryHelpers() {
    const std::string source = writeTempFile("/tmp/fuse_b79_helper_valid.obj", "# helper valid\n");
    desc.output_path = "/tmp/fuse_b79_helper_valid.fusemesh";

    expectTrue(cooked.ok, "seed cook for helper entry validation ok");

    valid.content_hash = cooked.content_hash;
    valid.source_path = source;
    valid.output_path = desc.output_path;
    valid.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(!fuse::project::is_invalid_cook_cache_entry(valid), "valid entry is not invalid");
    expectTrue(!fuse::project::is_stale_cook_cache_entry(valid), "fresh entry is not stale");

    fuse::project::CookCacheEntry invalid = valid;
    expectTrue(fuse::project::is_invalid_cook_cache_entry(invalid), "zero hash entry is invalid");
    expectTrue(!fuse::project::is_stale_cook_cache_entry(invalid), "invalid entry is not classified as stale");

    invalid = valid;
    invalid.source_path = "";
    expectTrue(fuse::project::is_invalid_cook_cache_entry(invalid), "empty source entry is invalid");

    fuse::project::CookCacheEntry stale_entry = valid;
    stale_entry.content_hash = cooked.content_hash + 1u;
    expectTrue(fuse::project::is_stale_cook_cache_entry(stale_entry),
               "mismatched stored hash is stale for readable source");

void testCookCacheHasInvalidAndStaleEntryGuards() {
    expectTrue(!cache.has_invalid_entries(), "empty cache has no invalid entries");
    expectTrue(!cache.has_stale_entries(), "empty cache has no stale entries");
    expectTrue(!cache.has_prunable_entries(), "empty cache has no prunable entries");

    const std::string source = writeTempFile("/tmp/fuse_b79_has_stale.obj", "# has stale v1\n");
    desc.output_path = "/tmp/fuse_b79_has_stale.fusemesh";

    expectTrue(cooked.ok, "seed cook for has_stale guard ok");

    writeTempFile(source, "# has stale v2\n");
    expectTrue(!cooker.cache().has_invalid_entries(), "content change does not create invalid entries");
    expectTrue(cooker.cache().has_stale_entries(), "content change marks cache stale");

    expectTrue(cooker.cache().prune_invalid_entries() == 0u, "prune_invalid skips stale-only cache");
    expectTrue(cooker.cache().prune_stale_entries() == 1u, "prune_stale removes stale-only entry");
void testContentHashValidityAndFnvGuards() {
    expectTrue(!fuse::project::is_valid_content_hash(0), "zero content hash is invalid");
    expectTrue(fuse::project::is_valid_content_hash(42u), "non-zero content hash is valid");

    const std::string source = writeTempFile("/tmp/fuse_b79_valid_hash.obj", "# valid hash\n");
    const fuse::u64 file_hash = fuse::project::hash_file_content(source);
    expectTrue(fuse::project::is_valid_content_hash(file_hash), "readable file yields valid content hash");
    expectTrue(!fuse::project::is_valid_content_hash(fuse::project::hash_file_content("")),
               "empty path yields invalid content hash");

    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 0) != 0, "null zero-length FNV is defined");
    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 4) == 0, "null non-zero-length FNV is guarded to zero");

void testCookCacheStaleVsInvalidClassification() {
    invalid.source_path = "/tmp/fuse_b79_classify_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_classify_invalid.fusemesh";
    expectTrue(fuse::project::is_invalid_cook_cache_entry(invalid), "zero-key entry is invalid");
    expectTrue(!fuse::project::is_stale_cook_cache_entry(invalid),
               "invalid entry is not classified as stale");

    const std::string source = writeTempFile("/tmp/fuse_b79_classify_stale.obj", "# classify v1\n");
    desc.output_path = "/tmp/fuse_b79_classify_stale.fusemesh";

    expectTrue(cooked.ok, "seed cook for classification ok");

    fuse::project::CookCacheEntry fresh;
    fresh.content_hash = cooked.content_hash;
    fresh.source_path = source;
    fresh.output_path = desc.output_path;
    fresh.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::is_valid_cook_cache_entry(fresh), "fresh entry is valid");
    expectTrue(!fuse::project::is_stale_cook_cache_entry(fresh), "fresh entry is not stale");

    writeTempFile(source, "# classify v2\n");
    expectTrue(fuse::project::is_stale_cook_cache_entry(fresh), "unchanged record is stale after source edit");
    expectTrue(!fuse::project::is_invalid_cook_cache_entry(fresh), "stale entry remains structurally valid");

void testCookCacheHasStaleInvalidAndCountGuards() {
    expectTrue(cache.count_prunable_entries() == 0u, "empty cache prunable count is zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_count_stale.obj", "# count v1\n");
    desc.output_path = "/tmp/fuse_b79_count_stale.fusemesh";

    expectTrue(cooked.ok, "seed cook for count guards ok");
    expectTrue(cooker.cache().count_prunable_entries() == 0u, "fresh cache prunable count is zero");
    expectTrue(!cooker.cache().has_stale_entries(), "fresh cache has no stale entries");
    expectTrue(!cooker.cache().has_invalid_entries(), "fresh cache has no invalid entries");

    writeTempFile(source, "# count v2\n");
    expectTrue(cooker.cache().has_stale_entries(), "source edit marks cache stale");
    expectTrue(!cooker.cache().has_invalid_entries(), "stale-only cache has no invalid entries");
    expectTrue(cooker.cache().count_prunable_entries() == 1u, "one stale entry counted");

    expectTrue(cooker.cache().prune_stale_entries() == 1u, "stale prune removes counted entry");
    expectTrue(cooker.cache().count_prunable_entries() == 0u, "clean cache prunable count returns to zero");
    expectTrue(cooker.cache().prune_stale_entries() == 0u, "second stale prune on clean cache is a no-op");

void testAssetCookerPruneStaleCache() {
    const std::string source = writeTempFile("/tmp/fuse_b79_cooker_prune.obj", "# cooker prune v1\n");
    desc.output_path = "/tmp/fuse_b79_cooker_prune.fusemesh";

    expectTrue(first.ok, "seed cook for cooker prune ok");
    expectTrue(cooker.prune_stale_cache() == 0u, "prune_stale_cache on fresh cache is a no-op");

    writeTempFile(source, "# cooker prune v2\n");
    expectTrue(cooker.cache().has_stale_entries(), "cooker cache stale before prune_stale_cache");
    expectTrue(cooker.prune_stale_cache() == 1u, "prune_stale_cache removes stale entry");
    expectTrue(cooker.cache().empty(), "cooker cache empty after prune_stale_cache");
    expectTrue(cooker.prune_stale_cache() == 0u, "second prune_stale_cache on empty cache is a no-op");

void testCookCachePruneInvalidEntriesOnLoad() {
    const std::string cachePath = "/tmp/fuse_b79_prune_invalid_load.json";
    {
        std::ofstream out(cachePath, std::ios::binary);
        out << R"({
  "schemaVersion": 1,
  "entries": [
      "contentHash": 101,
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_valid.fusemesh",
      "sourcePath": "/tmp/fuse_b79_valid.obj",
      "kind": "mesh"
    },
      "contentHash": 0,
      "outputPath": "/tmp/fuse_b79_zero_hash.fusemesh",
      "sourcePath": "/tmp/fuse_b79_zero_hash.obj",
    }
  ]
)";

    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "cache with missing on-disk source loads");
    expectTrue(loaded.entry_count() == 1u,
               "load keeps valid entry when source file is absent during stale reconcile");
    expectTrue(loaded.contains(101u), "absent-source entry remains addressable after load");

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

void testCookHashPreflightShouldSkip() {
    expectTrue(empty_path.should_skip(), "empty path preflight should_skip");
    expectTrue(empty_path.should_skip() == !empty_path.ok(), "should_skip mirrors !ok");

    const std::string source = writeTempFile("/tmp/fuse_b79_should_skip_mesh.obj", "# should skip mesh\n");
    desc.output_path = "/tmp/fuse_b79_should_skip_mesh.fusemesh";
    expectTrue(!mesh_preflight.should_skip(), "readable mesh preflight should not skip");

    const fuse::project::CookHashPreflight null_bytes =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(null_bytes.should_skip(), "null bytes preflight should_skip");

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

void testCookCacheInvalidationProbes() {
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

void testCookCachePruneReconcileEstimateGuards() {
    expectTrue(empty.total() == 0u, "empty cache prune estimate is zero");
    expectTrue(!cache.would_prune_all(), "empty cache would_prune_all is false");
    expectTrue(cache.count_stale_entries() == 0u, "empty cache stale count is zero");
    expectTrue(cache.probe_stale_content_sources().empty(), "empty cache stale source probe is empty");

    invalid.source_path = "/tmp/fuse_b79_est_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_est_invalid.fusemesh";
    expectTrue(cache.entry_count() == 0u, "invalid entry rejected during estimate setup");

    shader_entry.content_hash = 808;
    shader_entry.source_path = "/tmp/fuse_b79_est_shader.obj";
    shader_entry.output_path = "/tmp/fuse_b79_est_shader.fuseshader";
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

void testCookCacheProbeStaleContentSources() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_probe_stale_a.obj", "# probe stale a v1\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_probe_stale_b.obj", "# probe stale b v1\n");

    fuse::project::MeshImportDesc desc_a;
    desc_a.input_path = source_a;
    desc_a.output_path = "/tmp/fuse_b79_probe_stale_a.fusemesh";

    fuse::project::MeshImportDesc desc_b;
    desc_b.input_path = source_b;
    desc_b.output_path = "/tmp/fuse_b79_probe_stale_b.fusemesh";

    expectTrue(cooker.cook_mesh(desc_a).ok, "seed cook a for stale source probe");
    expectTrue(cooker.cook_mesh(desc_b).ok, "seed cook b for stale source probe");
    expectTrue(cooker.cache().probe_stale_content_sources().empty(), "fresh entries not probed as stale");

    writeTempFile(source_a, "# probe stale a v2\n");
    writeTempFile(source_b, "# probe stale b v2\n");

    const std::vector<std::string> stale_sources = cooker.cache().probe_stale_content_sources();
    expectTrue(stale_sources.size() == 2u, "two unique stale sources probed");
    expectTrue(stale_sources[0] == source_a || stale_sources[1] == source_a, "source a in stale probe");
    expectTrue(stale_sources[0] == source_b || stale_sources[1] == source_b, "source b in stale probe");

void testCookHashPreflightFnvAndCombineGuards() {
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

void testCookCacheWouldInvalidationProbes() {
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_down.fusemesh", {}, {}),

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would mesh\n");
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
               "would_invalidate_output reports seeded entry");
               "would_invalidate_stale_content with matching hash is false");

    writeTempFile(source, "# would mesh updated\n");
    const fuse::u64 updated_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(updated_hash != seeded.content_hash, "content change yields new hash key");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, updated_hash),
               "would_invalidate_stale_content with updated hash is true");
    expectTrue(cooker.cache().count_stale_content_for_source(source, updated_hash) == 1u,
               "count_stale_content matches would_invalidate_stale_content");

void testCookCacheEntryPreflightGuards() {
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
               "empty source path preflight reason");

    fuse::project::CookCacheEntry empty_output = valid;
    empty_output.output_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_output).reason ==
                   fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output path preflight reason");

                   fuse::project::CookHashRejectReason::ZeroContentHash)) == "zero_content_hash",
               "reject reason label for zero content hash");
void testCookCacheLookupPreflightGuards() {

    const auto zero_preflight = fuse::project::preflight_cook_cache_lookup(cache, 0u);
    expectTrue(zero_preflight.zero_key, "lookup preflight marks zero key");
    expectTrue(zero_preflight.should_skip(), "zero-key lookup preflight should skip");
    expectTrue(!zero_preflight.would_hit(), "zero-key lookup preflight would not hit");

    const auto empty_preflight = fuse::project::preflight_cook_cache_lookup(cache, 77u);
    expectTrue(empty_preflight.empty_cache, "lookup preflight marks empty cache");
    expectTrue(empty_preflight.would_miss(), "empty cache lookup preflight would miss");

    fuse::project::CookCacheEntry entry;
    entry.content_hash = 77u;
    entry.source_path = "/tmp/fuse_b79_preflight_lookup.obj";
    entry.output_path = "/tmp/fuse_b79_preflight_lookup.fusemesh";
    cache.store(entry);

    const auto hit_preflight = fuse::project::preflight_cook_cache_lookup(cache, 77u);
    expectTrue(hit_preflight.can_lookup(), "valid key passes lookup preflight");
    expectTrue(hit_preflight.would_hit(), "seeded entry would hit in preflight");
    expectTrue(cache.stats().hits == 0u, "lookup preflight does not touch hit stats");

void testCookCacheStorePreflightGuards() {
    invalid.source_path = "/tmp/fuse_b79_preflight_store.obj";
    invalid.output_path = "/tmp/fuse_b79_preflight_store.fusemesh";

    const auto zero_preflight = fuse::project::preflight_cook_cache_store(invalid);
    expectTrue(zero_preflight.zero_key, "store preflight marks zero key");
    expectTrue(zero_preflight.should_skip(), "zero-key store preflight should skip");

    fuse::project::CookCacheEntry empty_source = invalid;
    empty_source.content_hash = 88u;
    const auto empty_source_preflight = fuse::project::preflight_cook_cache_store(empty_source);
    expectTrue(empty_source_preflight.empty_source_path, "store preflight marks empty source path");
    expectTrue(empty_source_preflight.should_skip(), "empty source store preflight should skip");

    cache.store(empty_source);
    expectTrue(cache.entry_count() == 0u, "preflight-rejected store does not mutate cache");

void testCookImportHashPreflightGuards() {
    fuse::project::MeshImportDesc missing;
    missing.input_path = "/tmp/fuse_b79_preflight_missing.obj";
    missing.output_path = "/tmp/fuse_b79_preflight_missing.fusemesh";
    const auto unreadable = fuse::project::preflight_mesh_import(missing);
    expectTrue(unreadable.unreadable_source, "missing source fails import hash preflight");
    expectTrue(unreadable.should_skip(), "unreadable source import preflight should skip");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_ok.obj", "# preflight ok\n");
    fuse::project::MeshImportDesc valid;
    valid.input_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_ok.fusemesh";
    const auto ok = fuse::project::preflight_mesh_import(valid);
    expectTrue(ok.can_hash(), "readable source passes import hash preflight");
    expectTrue(fuse::project::preflight_cook_cache_key(0u, 42u).zero_source_hash,
               "cache key preflight marks zero source hash");
    expectTrue(fuse::project::preflight_cook_cache_key(99u, 42u).can_fold(),
               "valid source hash passes cache key preflight");

    const std::string sourceA = writeTempFile("/tmp/fuse_b79_probe_a.obj", "# probe a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_probe_b.obj", "# probe b\n");

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

    const fuse::project::CookBatchResult batch = cooker.cook_manifest(manifest);
    expectTrue(batch.ok, "probe test seeds cache");
    expectTrue(cooker.cache().entry_count() == 2u, "probe test has two cached entries");

    const fuse::u64 hits_before = cooker.cache().stats().hits;
    const fuse::u32 direct_probe = cooker.cache().probe_invalidate_source(sourceA);
    expectTrue(direct_probe == 1u, "probe counts direct source entry");
    expectTrue(cooker.cache().stats().hits == hits_before, "probe does not touch cache stats");

    writeTempFile(sourceA, "# probe a revised\n");
    const fuse::u32 stale_probe =
        cooker.cache().probe_stale_content_for_source(sourceA, batch.records[0].content_hash + 1u);
    expectTrue(stale_probe == 1u, "probe counts stale content entry");

    const auto upstream_probe = cooker.probe_upstream_dependency(manifest, sourceA);
    expectTrue(upstream_probe.would_invalidate(), "upstream probe reports invalidation scope");
    expectTrue(upstream_probe.direct_entries >= 1u, "upstream probe counts direct entries");
    expectTrue(upstream_probe.downstream_entries >= 1u, "upstream probe counts downstream cascade");

    const fuse::u32 removed = cooker.invalidate_upstream_dependency(manifest, sourceA);
    expectTrue(removed >= upstream_probe.total_entries(),
               "actual invalidation matches or exceeds probe estimate");

    cooker.cook_manifest(manifest);
    writeTempFile(sourceA, "# probe a reconcile\n");
    const auto estimate = cooker.estimate_stale_dependency_hashes(manifest);
    expectTrue(estimate.would_reconcile(), "reconcile estimate reports stale upstream hashes");
    expectTrue(!estimate.stale_source_paths.empty(), "reconcile estimate lists stale sources");
    expectTrue(estimate.stale_upstream_entries >= 1u, "reconcile estimate counts stale upstream entries");

    const fuse::u32 reconciled = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(reconciled >= estimate.total_entries(),
               "actual reconcile matches or exceeds estimate");

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

void testCookCacheHashPreflightGuards() {
    const fuse::project::CookCacheKeyPreflight zero_source =
        fuse::project::preflight_cook_cache_key(0, 42u);
    expectTrue(!zero_source.canCache(), "zero source hash fails cache-key preflight");
    expectTrue(zero_source.reason == fuse::project::CookCacheKeyRejectReason::ZeroSourceHash,
               "zero source hash reports ZeroSourceHash reason");

    const fuse::project::CookCacheKeyPreflight valid_source =
        fuse::project::preflight_cook_cache_key(99u, 0);
    expectTrue(valid_source.canCache(), "valid source-only fold passes cache-key preflight");
    expectTrue(valid_source.combined_key == 99u, "valid source-only preflight preserves combined key");

    const fuse::project::CookFileHashPreflight empty_path =
        fuse::project::preflight_file_content_hash("");
    expectTrue(!empty_path.canHash(), "empty path fails file hash preflight");
    expectTrue(empty_path.reason == fuse::project::CookFileHashRejectReason::EmptyPath,
               "empty path reports EmptyPath reason");

    const fuse::project::CookFileHashPreflight missing =
        fuse::project::preflight_file_content_hash("/tmp/fuse_b79_preflight_missing.obj");
    expectTrue(!missing.canHash(), "missing file fails file hash preflight");
    expectTrue(missing.reason == fuse::project::CookFileHashRejectReason::UnreadableSource,
               "missing file reports UnreadableSource reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_ok.obj", "# preflight ok\n");
    const fuse::project::CookFileHashPreflight readable =
        fuse::project::preflight_file_content_hash(source);
    expectTrue(readable.canHash(), "readable source passes file hash preflight");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 701;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_ok.fusemesh";
    const fuse::project::CookCacheKeyPreflight entry_preflight =
        fuse::project::preflight_cook_cache_entry(valid);
    expectTrue(entry_preflight.canCache(), "valid cache entry passes entry preflight");
    expectTrue(entry_preflight.combined_key == valid.content_hash,
               "entry preflight preserves stored combined key");

    fuse::project::CookCacheEntry invalid = valid;
    invalid.content_hash = 0;
    expectTrue(!fuse::project::preflight_cook_cache_entry(invalid).canCache(),
               "zero-hash entry fails entry preflight");
}

void testCookCachePruneEstimateAndProbes() {
    fuse::project::CookCache empty;
    expectTrue(empty.estimate_prunable_entries() == 0u, "empty cache prune estimate is zero");
    expectTrue(!empty.estimate_prune_all().would_prune(), "empty cache prune estimate would not prune");
    expectTrue(!empty.probe_invalidate_source("/tmp/fuse_b79_probe.obj").would_invalidate(),
               "empty cache source probe is guarded");

    const std::string source = writeTempFile("/tmp/fuse_b79_probe_stale.obj", "# probe v1\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_probe_stale.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for probe estimate ok");
    expectTrue(cooker.cache().estimate_prunable_entries() == 0u, "fresh cook cache has zero prune estimate");

    writeTempFile(source, "# probe v2\n");
    const fuse::project::CookCachePruneEstimate prune_estimate = cooker.cache().estimate_prune_all();
    expectTrue(prune_estimate.would_prune(), "stale entry marks prune estimate");
    expectTrue(prune_estimate.stale_count == 1u, "stale prune estimate counts one stale entry");
    expectTrue(cooker.cache().estimate_prunable_entries() == 1u, "estimate_prunable_entries matches stale count");

    const fuse::project::CookCacheInvalidationProbe stale_probe =
        cooker.cache().probe_stale_content_for_source(source, seeded.content_hash + 1u);
    expectTrue(stale_probe.would_invalidate(), "stale-content probe detects mismatched hash");
    expectTrue(stale_probe.affected_count == 1u, "stale-content probe counts one entry");
    expectTrue(cooker.cache().entry_count() == 1u, "stale-content probe does not mutate cache");

    const fuse::project::CookCacheInvalidationProbe source_probe =
        cooker.cache().probe_invalidate_source(source);
    expectTrue(source_probe.affected_count == 1u, "source probe counts seeded entry");
    expectTrue(cooker.cache().probe_invalidate_output(desc.output_path).affected_count == 1u,
               "output probe counts seeded entry");

    expectTrue(cooker.cache().probe_stale_content_for_source(source, 0u).affected_count == 0u,
               "zero current hash stale probe is guarded");
    expectTrue(cooker.cache().probe_stale_content_for_source("", seeded.content_hash).affected_count == 0u,
               "empty source stale probe is guarded");
}

} // namespace

int main() {
    fuse::core::initialize();

    testCombineCookCacheKeyGuards();
    testFnv1a64BytesEmptyGuard();
    testHashUpstreamDependenciesEmptyPathGuards();
    testHashManifestEntryEmptyDependencyGuards();
    testCookContentHashNullAndReadableGuards();
    testCookCacheInvalidAndStaleEntryHelpers();
    testCookCacheHasInvalidAndStaleEntryGuards();
    testCookCacheEmptyPathRejection();
    testCookCacheEmptyPathTextureAudioGuards();
    testCookCacheEmptyGuards();
    testCookCacheZeroKeyGuards();
    testCookCacheLookupOutEntryGuard();
    testCookCacheLookupEmptyCacheMissCounts();
    testCookCachePruneAll();
    testCookCacheStaleContentInvalidationGuards();
    testCookContentHashByteSpanGuards();
    testCookCacheHasPrunableEntriesAndCleanPruneGuards();
    testCookCacheInvalidVsStalePruneGuards();
    testCookCacheInvalidateUnknownHashGuards();
    testCookCacheLoadMissingFilePreservesEntries();
    testCookCacheShaderKindStalePrune();
    testCookCacheLoadPrunesStaleEntries();
    testCookCacheLoadPreservesEntriesWithoutOnDiskSource();
    testCookCacheLoadCorruptPreservesEntries();
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
    testContentHashValidityAndFnvGuards();
    testCookCacheStaleVsInvalidClassification();
    testCookCacheHasStaleInvalidAndCountGuards();
    testAssetCookerPruneStaleCache();
    testCookCachePruneInvalidEntriesOnLoad();
    testCookCacheLookupPreflightGuards();
    testCookCacheStorePreflightGuards();
    testCookImportHashPreflightGuards();
    testCookCacheInvalidationProbes();
    testCookCacheHashPreflightGuards();
    testCookCachePruneEstimateAndProbes();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
