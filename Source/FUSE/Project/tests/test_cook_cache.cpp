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
    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 4u) == 0u,
               "null data with non-zero size yields zero hash");
    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 4u) == 0,
               "null data with non-zero size hashes to zero");
    expectTrue(!fuse::project::preflight_fnv1a64_bytes(nullptr, 4u).ok(),
               "null data with non-zero size fails FNV preflight");
    expectTrue(fuse::project::preflight_fnv1a64_bytes(nullptr, 4u).reason ==
                   fuse::project::CookHashRejectReason::NullData,
               "null data FNV preflight reason is NullData");
    expectTrue(fuse::project::preflight_fnv1a64_bytes(nullptr, 0).ok(),
               "zero-size null data passes FNV preflight");
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

void testCookHashPreflightFnvAndCacheEntryGuards() {
    const fuse::project::CookHashPreflight null_data = fuse::project::preflight_fnv1a64_input(nullptr, 4u);
    expectTrue(!null_data.ok(), "null data with non-zero size fails FNV preflight");
    expectTrue(null_data.reason == fuse::project::CookHashRejectReason::NullData,
               "null data preflight reason is NullData");

    const fuse::project::CookHashPreflight empty_data = fuse::project::preflight_fnv1a64_input(nullptr, 0);
    expectTrue(empty_data.ok(), "null data with zero size passes FNV preflight");

    fuse::project::CookCacheEntry invalid_entry;
    invalid_entry.content_hash = 0;
    invalid_entry.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_entry.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight zero_entry =
        fuse::project::preflight_cook_cache_entry(invalid_entry);
    expectTrue(!zero_entry.ok(), "zero-hash cache entry fails preflight");
    expectTrue(zero_entry.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero-hash cache entry preflight reason");

    fuse::project::CookCacheEntry valid_entry = invalid_entry;
    valid_entry.content_hash = 88u;
    const fuse::project::CookHashPreflight valid_preflight =
        fuse::project::preflight_cook_cache_entry(valid_entry);
    expectTrue(valid_preflight.ok(), "valid cache entry passes preflight");

    const std::string tex_source = writeTempFile("/tmp/fuse_b79_preflight_tex.png", "PNG\n");
    fuse::project::TextureImportDesc tex;
    tex.input_path = tex_source;
    tex.output_path = "/tmp/fuse_b79_preflight_tex.fusetex";
    expectTrue(fuse::project::preflight_texture_import_hash(tex).ok(), "readable texture import passes preflight");

    const std::string audio_source = writeTempFile("/tmp/fuse_b79_preflight_audio.wav", "WAV\n");
    fuse::project::AudioImportDesc audio;
    audio.input_path = audio_source;
    audio.output_path = "/tmp/fuse_b79_preflight_audio.fuseaudio";
    expectTrue(fuse::project::preflight_audio_import_hash(audio).ok(), "readable audio import passes preflight");

    fuse::project::CookManifestEntry manifest_entry;
    manifest_entry.kind = fuse::project::CookAssetKind::Mesh;
    manifest_entry.source_path = tex_source;
    manifest_entry.output_path = "/tmp/fuse_b79_preflight_manifest.fusemesh";
    expectTrue(fuse::project::preflight_manifest_entry_hash(manifest_entry).ok(),
               "readable manifest entry passes preflight");
}

void testCookHashPreflightFnv1a64Guard() {
    const fuse::project::CookHashPreflight null_data =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_data.ok(), "null data with non-zero size fails fnv1a64 preflight");
    expectTrue(null_data.reason == fuse::project::CookHashRejectReason::NullData,
               "null data preflight reason is NullData");

    const fuse::project::CookHashPreflight empty_data =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 0);
    expectTrue(empty_data.ok(), "zero-size fnv1a64 preflight passes with null pointer");
    expectTrue(fuse::project::preflight_fnv1a64_bytes(nullptr, 0).ok(),
               "zero-size fnv1a64 preflight is stable");

    const char payload[] = "hash";
    const fuse::project::CookHashPreflight valid_data =
        fuse::project::preflight_fnv1a64_bytes(reinterpret_cast<const fuse::u8*>(payload),
                                               sizeof(payload) - 1);
    expectTrue(valid_data.ok(), "non-null fnv1a64 preflight passes for valid buffer");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 201;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(),
               "valid cache entry passes preflight");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    expectTrue(!fuse::project::preflight_cook_cache_entry(zero_key).ok(),
               "zero content hash fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(zero_key).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero key cache entry preflight reason");

    fuse::project::CookCacheEntry empty_source = valid;
    empty_source.source_path = "";
    expectTrue(!fuse::project::preflight_cook_cache_entry(empty_source).ok(),
               "empty source path fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source cache entry preflight reason");

    fuse::project::CookCacheEntry empty_output = valid;
    empty_output.output_path = "";
    expectTrue(!fuse::project::preflight_cook_cache_entry(empty_output).ok(),
               "empty output path fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_output).reason ==
                   fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output cache entry preflight reason");
}

void testCookHashPreflightFnv1a64Guard() {
    const fuse::project::CookHashPreflight null_data =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_data.ok(), "null data with non-zero size fails fnv1a64 preflight");
    expectTrue(null_data.reason == fuse::project::CookHashRejectReason::NullData,
               "null data preflight reason is NullData");

    const fuse::project::CookHashPreflight empty_data =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 0);
    expectTrue(empty_data.ok(), "zero-size fnv1a64 preflight passes with null pointer");
    expectTrue(fuse::project::preflight_fnv1a64_bytes(nullptr, 0).ok(),
               "zero-size fnv1a64 preflight is stable");

    const char payload[] = "hash";
    const fuse::project::CookHashPreflight valid_data =
        fuse::project::preflight_fnv1a64_bytes(reinterpret_cast<const fuse::u8*>(payload),
                                               sizeof(payload) - 1);
    expectTrue(valid_data.ok(), "non-null fnv1a64 preflight passes for valid buffer");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 201;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(),
               "valid cache entry passes preflight");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    expectTrue(!fuse::project::preflight_cook_cache_entry(zero_key).ok(),
               "zero content hash fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(zero_key).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero key cache entry preflight reason");

    fuse::project::CookCacheEntry empty_source = valid;
    empty_source.source_path = "";
    expectTrue(!fuse::project::preflight_cook_cache_entry(empty_source).ok(),
               "empty source path fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source cache entry preflight reason");

    fuse::project::CookCacheEntry empty_output = valid;
    empty_output.output_path = "";
    expectTrue(!fuse::project::preflight_cook_cache_entry(empty_output).ok(),
               "empty output path fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_output).reason ==
                   fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output cache entry preflight reason");
}

void testCookHashPreflightFnvAndCombineGuards() {
    const fuse::project::CookHashPreflight null_bytes = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_bytes.ok(), "null data with non-zero size fails FNV preflight");
    expectTrue(null_bytes.reason == fuse::project::CookHashRejectReason::NullData,
               "null data FNV preflight reason is NullData");

    const fuse::project::CookHashPreflight empty_bytes = fuse::project::preflight_fnv1a64_bytes(nullptr, 0);
    expectTrue(empty_bytes.ok(), "null data with zero size passes FNV preflight");

    const fuse::u8 sample[] = {0x42};
    expectTrue(fuse::project::preflight_fnv1a64_bytes(sample, 1).ok(), "non-null data passes FNV preflight");

    const fuse::project::CookHashPreflight zero_combine =
        fuse::project::preflight_combine_cook_cache_key(0, 42u);
    expectTrue(!zero_combine.ok(), "zero source fails combine cache key preflight");
    expectTrue(zero_combine.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero source combine preflight reason");

    expectTrue(fuse::project::preflight_combine_cook_cache_key(99u, 0).ok(),
               "valid source-only combine preflight passes");
    expectTrue(fuse::project::preflight_combine_cook_cache_key(99u, 42u).ok(),
               "valid combined fold preflight passes");
}

void testCookHashPreflightGuards() {
    const fuse::project::CookHashPreflight null_fnv = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_fnv.ok(), "null data with non-zero size fails FNV preflight");
    expectTrue(null_fnv.reason == fuse::project::CookHashRejectReason::NullData,
               "null FNV preflight reason is NullData");

    const fuse::project::CookHashPreflight empty_fnv = fuse::project::preflight_fnv1a64_bytes(nullptr, 0);
    expectTrue(empty_fnv.ok(), "null data with zero size passes FNV preflight");
    expectTrue(fuse::project::is_valid_fnv1a64_input(nullptr, 0),
               "null data with zero size passes FNV input validation");

    const fuse::project::CookHashPreflight null_fnv = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_fnv.ok(), "null data with non-zero size fails FNV preflight");
    expectTrue(null_fnv.reason == fuse::project::CookHashRejectReason::NullData,
               "null FNV preflight reason is NullData");
    expectTrue(fuse::project::preflight_fnv1a64_bytes(nullptr, 0).ok(),
               "null data with zero size passes FNV preflight");
    expectTrue(fuse::project::fnv1a64_bytes(nullptr, 4u) == 14695981039346656037ull,
               "null data with non-zero size returns FNV offset basis without crashing");

    const fuse::project::CookHashPreflight non_cacheable =
        fuse::project::preflight_cacheable_cook_cache_key(0, 42u);
    expectTrue(!non_cacheable.ok(), "zero source hash fails cacheable key preflight");
    expectTrue(non_cacheable.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "cacheable key preflight propagates zero source reason");

    const fuse::project::CookHashPreflight cacheable =
        fuse::project::preflight_cacheable_cook_cache_key(99u, 42u);
    expectTrue(cacheable.ok(), "valid combined fold passes cacheable key preflight");
    const fuse::project::CookHashPreflight null_bytes = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_bytes.ok(), "null bytes preflight rejects non-zero size");
    expectTrue(null_bytes.reason == fuse::project::CookHashRejectReason::NullData,
               "null bytes preflight reason is NullData");
               "zero-size null bytes preflight passes");
               "null data FNV preflight reason is NullData");

    const fuse::u8 byte = 42;
    expectTrue(fuse::project::preflight_fnv1a64_bytes(&byte, 1u).ok(),
               "non-null data passes FNV preflight");
    expectTrue(!null_bytes.ok(), "null data with non-zero size fails byte hash preflight");
               "null byte preflight reason is NullData");

    const fuse::project::CookHashPreflight valid_bytes = fuse::project::preflight_fnv1a64_bytes(&byte, 1u);
    expectTrue(valid_bytes.ok(), "non-null byte buffer passes hash preflight");
    const fuse::project::CookHashPreflight null_bytes =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_bytes.ok(), "null data with non-zero size fails FNV preflight");
               "null data preflight reason is NullData");

    const fuse::u8 byte = 0x2a;
               "valid FNV preflight accepts non-null buffer");
               "valid FNV preflight accepts zero-size null buffer");
    expectTrue(!null_fnv.ok(), "null FNV preflight rejects non-zero size with null data");

               "valid FNV preflight accepts non-null data");
               "valid FNV preflight accepts zero-size null data");

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

void testCookCacheWouldInvalidateProbes() {
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_src.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_src.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_out.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");
    expectTrue(cache.probe_downstream_sources("/tmp/fuse_b79_would_out.fusemesh", {}, {}).empty(),
               "probe_downstream on empty cache returns empty list");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would invalidate\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content with matching hash is false");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content with mismatched hash is true");

    writeTempFile(source, "# would invalidate updated\n");
    expectTrue(cooker.cache().count_stale_entries() == 1u, "count_stale_entries reports stale valid entry");
    expectTrue(cooker.cache().count_prunable_entries() == 1u, "stale entry is also prunable");
    expectTrue(cooker.cache().count_invalid_entries() == 0u, "stale valid entry is not invalid");

    const fuse::u32 removed = cooker.cache().prune_stale_entries();
    expectTrue(removed == 1u, "prune removes probed stale entry");
    expectTrue(cooker.cache().count_stale_entries() == 0u, "count_stale_entries zero after prune");
    const fuse::project::CookHashPreflight null_fnv =
    const fuse::project::CookHashPreflight null_fnv = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_fnv.ok(), "null data with non-zero size fails FNV preflight");
    expectTrue(null_fnv.reason == fuse::project::CookHashRejectReason::NullData,
               "null FNV preflight reason is NullData");
    expectTrue(fuse::project::preflight_fnv1a64_bytes(nullptr, 0).ok(),
               "null data with zero size passes FNV preflight");

    fuse::project::TextureImportDesc tex;
    tex.input_path = source;
    const fuse::project::CookHashPreflight null_data =
    expectTrue(!null_data.ok(), "null data with non-zero size fails FNV preflight");
    expectTrue(null_data.reason == fuse::project::CookHashRejectReason::NullData,
               "null data preflight reason is NullData");

    const fuse::u8 byte = 0x2a;
    expectTrue(fuse::project::preflight_fnv1a64_bytes(&byte, 1u).ok(),
               "valid FNV input passes preflight");

    const std::string tex_source = writeTempFile("/tmp/fuse_b79_preflight_tex.png", "# tex\n");
    tex.input_path = tex_source;
    tex.output_path = "/tmp/fuse_b79_preflight_tex.fusetex";
    expectTrue(fuse::project::preflight_texture_import_hash(tex).ok(),
               "readable texture import passes hash preflight");

    tex.output_path = "";
    expectTrue(fuse::project::preflight_texture_import_hash(tex).reason ==
                   fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty texture output path preflight reason");

    fuse::project::CookManifestEntry manifest_entry;
    manifest_entry.source_path = source;
    manifest_entry.output_path = "/tmp/fuse_b79_preflight_manifest.fusemesh";
    expectTrue(fuse::project::preflight_manifest_entry_hash(manifest_entry).ok(),
               "readable manifest entry passes hash preflight");
    manifest_entry.source_path = "";
    expectTrue(fuse::project::preflight_manifest_entry_hash(manifest_entry).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty manifest source path preflight reason");
}

void testCookCacheReconcileEstimators() {
    const fuse::project::CookCacheReconcileEstimate empty_estimate = cache.estimate_reconcile();
    expectTrue(empty_estimate.invalid_entries == 0u, "empty cache reconcile invalid count is zero");
    expectTrue(empty_estimate.stale_entries == 0u, "empty cache reconcile stale count is zero");
    expectTrue(empty_estimate.prunable_entries == 0u, "empty cache reconcile prunable count is zero");
    expectTrue(cache.estimate_prune_all() == 0u, "estimate_prune_all on empty cache is zero");
    expectTrue(cache.estimate_prune_stale_entries() == 0u,
               "estimate_prune_stale on empty cache is zero");
    expectTrue(cache.estimate_prune_invalid_entries() == 0u,
               "estimate_prune_invalid on empty cache is zero");
    expectTrue(cache.count_stale_entries() == 0u, "count_stale on empty cache is zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_reconcile_mesh.obj", "# reconcile v1\n");
    desc.output_path = "/tmp/fuse_b79_reconcile_mesh.fusemesh";

    expectTrue(seeded.ok, "seed cook for reconcile estimators ok");

    const fuse::project::CookCacheReconcileEstimate fresh_estimate = cooker.cache().estimate_reconcile();
    expectTrue(fresh_estimate.invalid_entries == 0u, "fresh cache reconcile invalid count is zero");
    expectTrue(fresh_estimate.stale_entries == 0u, "fresh cache reconcile stale count is zero");
    expectTrue(fresh_estimate.prunable_entries == 0u, "fresh cache reconcile prunable count is zero");
    expectTrue(cooker.cache().estimate_prune_all() == 0u, "estimate_prune_all on fresh cache is zero");

    writeTempFile(source, "# reconcile v2\n");
    const fuse::project::CookCacheReconcileEstimate stale_estimate = cooker.cache().estimate_reconcile();
    expectTrue(stale_estimate.stale_entries == 1u, "stale reconcile reports one stale entry");
    expectTrue(stale_estimate.prunable_entries == 1u, "stale reconcile reports one prunable entry");
    expectTrue(cooker.cache().count_stale_entries() == 1u, "count_stale reports one stale entry");
    expectTrue(cooker.cache().estimate_prune_stale_entries() == 1u,
               "estimate_prune_stale matches count_stale");

    const fuse::u32 removed = cooker.cache().prune_all();
    expectTrue(removed == stale_estimate.prunable_entries,
               "prune_all removes reconcile-estimated prunable count");
    expectTrue(cooker.cache().estimate_reconcile().prunable_entries == 0u,
               "reconcile prunable count zero after prune");
    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::NonCacheableKey)) == "non_cacheable_key",
               "reject reason label for non-cacheable key");
        fuse::project::preflight_fnv1a64_input(nullptr, 4u);
    expectTrue(!null_fnv.ok(), "null FNV input fails preflight");
               "null FNV input preflight reason");

    const fuse::project::CookHashPreflight empty_fnv = fuse::project::preflight_fnv1a64_input(nullptr, 0);
    expectTrue(empty_fnv.ok(), "zero-size null FNV input passes preflight");

    fuse::project::CookCacheEntry invalid_entry;
    invalid_entry.content_hash = 0;
    invalid_entry.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_entry.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight invalid_entry_preflight =
        fuse::project::preflight_cook_cache_entry(invalid_entry);
    expectTrue(!invalid_entry_preflight.ok(), "zero-hash cache entry fails preflight");
    expectTrue(invalid_entry_preflight.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero-hash cache entry preflight reason");

    fuse::project::CookCacheEntry valid_entry = invalid_entry;
    valid_entry.content_hash = 88u;
    const fuse::project::CookHashPreflight valid_entry_preflight =
        fuse::project::preflight_cook_cache_entry(valid_entry);
    expectTrue(valid_entry_preflight.ok(), "valid cache entry passes preflight");
    expectTrue(fuse::project::is_valid_cook_cache_entry(valid_entry),
               "preflight success matches entry validation");

    const std::string tex_source = writeTempFile("/tmp/fuse_b79_preflight_tex.png", "PNG\n");
    fuse::project::TextureImportDesc tex_desc;
    tex_desc.input_path = tex_source;
    tex_desc.output_path = "/tmp/fuse_b79_preflight_tex.fusetex";
    expectTrue(fuse::project::preflight_texture_import_hash(tex_desc).ok(),

    const std::string audio_source = writeTempFile("/tmp/fuse_b79_preflight_audio.wav", "WAV\n");
                   fuse::project::CookHashRejectReason::NullData)) == "null_data",
               "reject reason label for null data");

    const std::string texture_source = writeTempFile("/tmp/fuse_b79_preflight_tex.png", "# preflight tex\n");
    tex_desc.input_path = texture_source;

    const std::string audio_source = writeTempFile("/tmp/fuse_b79_preflight_audio.wav", "# preflight audio\n");

    const std::string audio_source = writeTempFile("/tmp/fuse_b79_preflight_audio.wav", "# audio\n");

    fuse::project::AudioImportDesc audio_desc;
    audio_desc.input_path = audio_source;
    audio_desc.output_path = "/tmp/fuse_b79_preflight_audio.fuseaudio";
    expectTrue(fuse::project::preflight_audio_import_hash(audio_desc).ok(),
               "readable audio import passes hash preflight");

    manifest_entry.kind = fuse::project::CookAssetKind::Mesh;
    manifest_entry.source_path = tex_source;
    expectTrue(!null_bytes.ok(), "null byte buffer fails FNV preflight");
    expectTrue(null_bytes.reason == fuse::project::CookHashRejectReason::NullData,
               "null byte buffer preflight reason is NullData");
               "zero-size null buffer passes FNV preflight");

    const std::string dep = writeTempFile("/tmp/fuse_b79_preflight_dep.obj", "# preflight dep\n");
    fuse::project::CookManifestEntry entry_with_dep;
    entry_with_dep.kind = fuse::project::CookAssetKind::Mesh;
    entry_with_dep.source_path = source;
    entry_with_dep.output_path = "/tmp/fuse_b79_preflight_mesh.fusemesh";
    entry_with_dep.dependencies.push_back("/tmp/fuse_b79_preflight_missing.fusemesh");

    fuse::project::CookManifest manifest_with_unreadable_dep;
    fuse::project::CookManifestEntry unreadable_dep;
    unreadable_dep.kind = fuse::project::CookAssetKind::Mesh;
    unreadable_dep.source_path = "/tmp/fuse_b79_preflight_missing_dep.obj";
    unreadable_dep.output_path = "/tmp/fuse_b79_preflight_missing.fusemesh";
    manifest_with_unreadable_dep.assets.push_back(unreadable_dep);

    expectTrue(!fuse::project::preflight_manifest_entry_with_upstream(entry_with_dep,
                                                                     manifest_with_unreadable_dep)
                   .ok(),
               "manifest entry with unreadable dependency source fails upstream preflight");
    expectTrue(fuse::project::preflight_manifest_entry_with_upstream(entry_with_dep,
                   .reason == fuse::project::CookHashRejectReason::SourceUnreadable,
               "unreadable dependency source preflight reason is SourceUnreadable");

    fuse::project::CookManifest manifest_with_dep;
    fuse::project::CookManifestEntry dep_asset;
    dep_asset.kind = fuse::project::CookAssetKind::Mesh;
    dep_asset.source_path = dep;
    dep_asset.output_path = "/tmp/fuse_b79_preflight_dep.fusemesh";
    manifest_with_dep.assets.push_back(dep_asset);
    entry_with_dep.dependencies[0] = dep_asset.output_path;

    expectTrue(fuse::project::preflight_manifest_entry_with_upstream(entry_with_dep, manifest_with_dep)
               "manifest entry with readable dependency passes upstream preflight");
    const std::string tex_source = writeTempFile("/tmp/fuse_b79_preflight_tex.png", "# preflight tex\n");
                   fuse::project::CookHashRejectReason::UnknownDependencyOutput)) == "unknown_dependency_output",
               "reject reason label for unknown dependency output");

    expectTrue(fuse::project::preflight_texture_import_hash(tex).ok(), "readable texture import passes preflight");

                   fuse::project::CookHashRejectReason::UnresolvedDependency)) == "unresolved_dependency",
               "reject reason label for unresolved dependency");


    tex.input_path = "";
               "empty texture input path preflight reason");


    fuse::project::AudioImportDesc audio;
    audio.input_path = audio_source;
    audio.output_path = "/tmp/fuse_b79_preflight_audio.fuseaudio";
    expectTrue(fuse::project::preflight_audio_import_hash(audio).ok(), "readable audio import passes preflight");

               "readable manifest entry passes preflight");

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    invalid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(!fuse::project::preflight_cook_cache_entry(invalid).ok(),
               "zero content hash fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_entry_preflight_ok.obj", "# entry preflight\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 202;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_entry_preflight_ok.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(),
               "valid mesh cache entry passes preflight");
    const fuse::project::CookHashPreflight combine_zero =
        fuse::project::preflight_combine_cook_cache_key(0, 42u);
    expectTrue(!combine_zero.ok(), "zero source fails combine cache key preflight");
    expectTrue(fuse::project::preflight_combine_cook_cache_key(99u, 42u).ok(),
               "valid source and upstream pass combine cache key preflight");


    audio.input_path = source;
    expectTrue(fuse::project::preflight_audio_import_hash(audio).ok(),

    manifest_entry.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";






    expectTrue(fuse::project::preflight_texture_import_hash(tex).ok(), "texture import preflight ok");

    expectTrue(fuse::project::preflight_audio_import_hash(audio).ok(), "audio import preflight ok");

    fuse::project::CookCacheEntry valid_entry;
    valid_entry.content_hash = 808;
    valid_entry.source_path = source;
    valid_entry.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid_entry).ok(),
               "valid cache entry passes preflight");

    fuse::project::CookCacheEntry zero_entry = valid_entry;
    zero_entry.content_hash = 0;
    expectTrue(fuse::project::preflight_cook_cache_entry(zero_entry).reason ==
    fuse::project::CookManifestEntry dep_entry;
    dep_entry.kind = fuse::project::CookAssetKind::Mesh;
    dep_entry.source_path = dep;
    dep_entry.output_path = "/tmp/fuse_b79_preflight_dep.fusemesh";
    manifest.assets.push_back(dep_entry);

    expectTrue(!fuse::project::preflight_upstream_dependencies_hash({"/tmp/fuse_b79_missing_dep.fusemesh"}, manifest)
               "unknown dependency output fails upstream preflight");
    expectTrue(fuse::project::preflight_upstream_dependencies_hash({"/tmp/fuse_b79_missing_dep.fusemesh"}, manifest)
                       .reason == fuse::project::CookHashRejectReason::UnknownDependencyOutput,
               "unknown dependency output preflight reason");

    fuse::project::CookManifestEntry with_dep;
    with_dep.kind = fuse::project::CookAssetKind::Mesh;
    with_dep.source_path = dep;
    with_dep.output_path = "/tmp/fuse_b79_preflight_with_dep.fusemesh";
    with_dep.dependencies.push_back(dep_entry.output_path);
    expectTrue(fuse::project::preflight_manifest_entry_hash(with_dep, manifest).ok(),
               "manifest entry with resolved dependency passes preflight");

void testCookCachePruneReconcileEstimate() {
    const fuse::project::CookCachePruneEstimate empty_estimate = cache.estimate_prune_reconcile();
    expectTrue(empty_estimate.invalid_count == 0u && empty_estimate.stale_count == 0u,
               "empty cache prune estimate is zero");
    expectTrue(empty_estimate.total() == 0u, "empty cache prune total is zero");
    expectTrue(cache.count_stale_entries() == 0u, "empty cache stale count is zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_prune_est.obj", "# prune est v1\n");
    desc.output_path = "/tmp/fuse_b79_prune_est.fusemesh";

    expectTrue(seeded.ok, "seed cook for prune estimate ok");

    const fuse::project::CookCachePruneEstimate fresh_estimate = cooker.cache().estimate_prune_reconcile();
    expectTrue(fresh_estimate.total() == 0u, "fresh cache prune estimate is zero");
    expectTrue(cooker.cache().count_stale_entries() == 0u, "fresh cache has no stale entries");

    writeTempFile(source, "# prune est v2\n");
    const fuse::project::CookCachePruneEstimate stale_estimate = cooker.cache().estimate_prune_reconcile();
    expectTrue(stale_estimate.stale_count == 1u, "stale estimate reports one stale entry");
    expectTrue(stale_estimate.invalid_count == 0u, "stale-only estimate has zero invalid entries");
    expectTrue(stale_estimate.total() == cooker.cache().count_prunable_entries(),
               "prune estimate total matches prunable count");

    expectTrue(removed == stale_estimate.total(), "prune_all removes estimated total");
               "null FNV input preflight reason is NullData");
               "zero-length null FNV input passes preflight");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_preflight_mesh.fusemesh";
    entry.dependencies.push_back(dep);
    expectTrue(fuse::project::preflight_manifest_entry_hash(entry).ok(),
               "manifest entry with readable dependency passes preflight");

    entry.dependencies.push_back("/tmp/fuse_b79_missing_preflight_dep.obj");
    expectTrue(!fuse::project::preflight_manifest_entry_hash(entry).ok(),
               "manifest entry with unreadable dependency fails preflight");
    expectTrue(fuse::project::preflight_manifest_entry_hash(entry).reason ==
                   fuse::project::CookHashRejectReason::SourceUnreadable,
               "unreadable dependency preflight reason is SourceUnreadable");
    entry.output_path = desc.output_path;
               "manifest entry preflight passes for readable source");
    expectTrue(fuse::project::preflight_manifest_entry_with_upstream_hash(entry, manifest).ok(),
               "manifest entry with empty deps passes upstream preflight");

    entry.dependencies.push_back("/tmp/fuse_b79_missing_upstream.fusemesh");
    fuse::project::CookManifestEntry upstream;
    upstream.kind = fuse::project::CookAssetKind::Mesh;
    upstream.source_path = "/tmp/fuse_b79_missing_upstream.obj";
    upstream.output_path = entry.dependencies.front();
    manifest.assets.push_back(upstream);
    expectTrue(!fuse::project::preflight_manifest_entry_with_upstream_hash(entry, manifest).ok(),
               "manifest entry with unreadable upstream fails preflight");
    manifest_entry.source_path = texture_source;
        fuse::project::preflight_fnv1a64_bytes(nullptr, 8u);
    expectTrue(!null_fnv.ok(), "null FNV input with non-zero size fails preflight");
               "null FNV input with zero size passes preflight");

    const fuse::project::CookHashPreflight combine_preflight =
        fuse::project::preflight_combine_cook_cache_key(99u, 42u);
    expectTrue(combine_preflight.ok(), "valid source and upstream pass combine preflight");
    expectTrue(!fuse::project::preflight_combine_cook_cache_key(0, 42u).ok(),
               "zero source fails combine preflight");

    dep_entry.source_path = source;
    dep_entry.dependencies = {"/tmp/fuse_b79_missing_dep.obj"};
    expectTrue(!fuse::project::preflight_manifest_entry_dependencies(dep_entry).ok(),
               "unreadable manifest dependency fails entry dependency preflight");
    expectTrue(fuse::project::preflight_manifest_entry_dependencies(dep_entry).reason ==
               "missing dependency preflight reason is SourceUnreadable");

    dep_entry.dependencies.clear();
    expectTrue(fuse::project::preflight_manifest_entry_dependencies(dep_entry).ok(),
               "manifest entry without dependencies passes dependency preflight");
    expectTrue(fuse::project::preflight_manifest_entry_dependencies(entry).ok(),
    entry.dependencies.push_back("/tmp/fuse_b79_missing_dep_preflight.obj");
    expectTrue(!fuse::project::preflight_manifest_entry_dependencies(entry).ok(),
               "unreadable dependency fails manifest entry dependency preflight");
    expectTrue(fuse::project::preflight_manifest_entry_dependencies(entry).reason ==
    fuse::project::CookManifestEntry manifest_entry;
    expectTrue(fuse::project::preflight_manifest_entry_hash(manifest_entry).ok(),
               "readable manifest entry passes hash preflight");

    fuse::project::CookCacheEntry cache_entry;
    cache_entry.content_hash = 0;
    cache_entry.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    cache_entry.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(!fuse::project::preflight_cook_cache_entry(cache_entry).ok(),
    expectTrue(fuse::project::preflight_cook_cache_entry(cache_entry).reason ==
               "zero content hash cache entry preflight reason");

    cache_entry.content_hash = 88u;
    expectTrue(fuse::project::preflight_cook_cache_entry(cache_entry).ok(),
    manifest_entry.source_path = source;
    manifest_entry.output_path = "/tmp/fuse_b79_preflight_manifest.fusemesh";
    audio.output_path = "";
    expectTrue(fuse::project::preflight_audio_import_hash(audio).reason ==
                   fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty audio output path preflight reason");

    manifest_entry.kind = fuse::project::CookAssetKind::Mesh;
    manifest_entry.source_path = tex_source;

    const fuse::project::CookHashPreflight unresolved_upstream =
        fuse::project::preflight_upstream_dependencies_hash({"/tmp/fuse_b79_missing_dep.fusemesh"}, manifest);
    expectTrue(!unresolved_upstream.ok(), "unresolved dependency output fails upstream preflight");
    expectTrue(unresolved_upstream.reason == fuse::project::CookHashRejectReason::UnresolvedDependency,
               "unresolved dependency preflight reason");
}

void testCookCacheWouldInvalidateSourceOutputGuards() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(!cache.would_invalidate_output(""), "would_invalidate_output rejects empty path");
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("", 42u),
               "would_invalidate_stale_content rejects empty source");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 0u),
               "would_invalidate_stale_content rejects zero hash");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({}),
               "would_invalidate_stale_upstream on empty list is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_down.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");
}

void testCookCacheWouldInvalidatePathProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_down.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_path.obj", "# would path\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_path.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate path probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_key;
    invalid_key.content_hash = 0;
    invalid_key.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_key.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(!fuse::project::preflight_cook_cache_entry(invalid_key).ok(),
               "zero content hash fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid_key).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash preflight reason");

    fuse::project::CookCacheEntry empty_source;
    empty_source.content_hash = 42;
    empty_source.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry.obj", "# entry preflight\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "readable mesh entry passes preflight");

    valid.kind = fuse::project::CookAssetKind::Shader;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "shader entry passes structural preflight");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_down.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would invalidate\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");

    writeTempFile(source, "# would invalidate updated\n");
    expectTrue(cooker.cache().would_prune_all(), "would_prune_all true after source change");
}

void testCookCacheInvalidationProbes() {
    expectTrue(!cache.would_invalidate(42u), "would_invalidate on empty cache is false");
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_probe.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_probe.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_probe.obj", 1u),
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_probe.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_probe.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_probe.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_probe_out.fusemesh"),
    expectTrue(cache.probe_stale_content_sources().empty(),
               "probe_stale_content on empty cache returns empty list");
    expectTrue(cache.estimate_prune_reconcile() == 0u, "estimate_prune_reconcile on empty cache is zero");
    expectTrue(cache.count_by_source("/tmp/fuse_b79_probe.obj") == 0u,
               "count_by_source on empty cache returns zero");
    expectTrue(cache.count_prunable_entries() == 0u, "count_prunable on empty cache returns zero");
    expectTrue(cache.count_stale_entries() == 0u, "count_stale on empty cache returns zero");
    expectTrue(cache.count_invalid_entries() == 0u, "count_invalid on empty cache returns zero");
    expectTrue(cache.count_stale_entries() == 0u, "count_stale on empty cache returns zero");
    expectTrue(cache.count_invalidate_all() == 0u, "count_invalidate_all on empty cache returns zero");
    expectTrue(cache.estimate_prune_all() == 0u, "estimate_prune_all on empty cache returns zero");
    expectTrue(cache.probe_stale_content_sources().empty(),
               "probe_stale_content on empty cache returns empty list");
    expectTrue(cache.probe_stale_upstream_sources({{"/tmp/fuse_b79_probe.obj", 1u}}).empty(),
               "probe_stale_upstream on empty cache returns empty list");
    expectTrue(cache.probe_unique_stale_upstream_sources({{"/tmp/fuse_b79_probe.obj", 1u}}).empty(),
               "probe_unique_stale_upstream on empty cache returns empty list");

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
    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source reports seeded source");
               "would_invalidate_output reports seeded output");
    expectTrue(!cooker.cache().would_invalidate_source(""),
               "would_invalidate_source rejects empty path");
    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_output(""),
               "would_invalidate_output rejects empty path");
    expectTrue(cooker.cache().count_by_source(source) == 1u, "count_by_source finds seeded entry");
    expectTrue(cooker.cache().count_by_output(desc.output_path) == 1u, "count_by_output finds seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content with matching hash is false");
    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded source");
    expectTrue(cooker.cache().count_invalidate_all() == 1u, "count_invalidate_all reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source finds seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path), "would_invalidate_output finds seeded entry");
               "would_invalidate_stale_content false for matching hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true for mismatched hash");
    expectTrue(cooker.cache().count_stale_content_for_source(source, seeded.content_hash) == 0u,
               "count_stale_content with matching hash returns zero");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false with matching hash");
    expectTrue(cooker.cache().count_stale_content_for_source(source, seeded.content_hash + 1u) == 1u,
               "count_stale_content with mismatched hash returns one");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true with mismatched hash");
               "would_invalidate_stale_content reports mismatched hash");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content rejects matching hash");
               "would_invalidate_stale_content with matching hash is false");
               "would_invalidate_stale_content with mismatched hash is true");
    expectTrue(cooker.cache().probe_stale_content_sources({{source, seeded.content_hash}}).empty(),
               "probe_stale_content with matching hash returns empty list");
    expectTrue(cooker.cache().probe_stale_content_sources({{source, seeded.content_hash + 1u}}).size() == 1u,
               "probe_stale_content with mismatched hash returns one source");
    expectTrue(!cooker.cache().would_invalidate_stale_upstream_hashes({{source, 0u}}),
               "would_invalidate_stale_upstream with matching upstream is false");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded source");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_source("/tmp/fuse_b79_unknown_source.obj"),
               "would_invalidate_source rejects unknown source");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded output");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output rejects empty path");

    const std::vector<std::string> stale_content_probe =
        cooker.cache().probe_stale_content_sources({{source, seeded.content_hash + 1u}});
    expectTrue(stale_content_probe.size() == 1u, "probe_stale_content finds mismatched hash");
    expectTrue(stale_content_probe.front() == source, "probe_stale_content returns stale source path");
               "probe_stale_content empty when hash matches");

    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded source");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output rejects empty path");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded output");
    expectTrue(!cooker.cache().would_invalidate_stale_content(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");
    expectTrue(!cooker.cache().would_invalidate_stale_upstream({{source, 0u}}),
               "would_invalidate_stale_upstream false when upstream hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_upstream({{source, 42u}}),
               "would_invalidate_stale_upstream true when upstream hash mismatches");

    writeTempFile(source, "# probe mesh updated\n");
    expectTrue(cooker.cache().count_stale_entries() == 1u, "count_stale reports content-drift entry");
    expectTrue(cooker.cache().count_stale_entries() == 1u, "count_stale_entries reports content drift");
    expectTrue(cooker.cache().count_prunable_entries() == 1u, "count_prunable reports stale entry");
    expectTrue(cooker.cache().count_stale_entries() == 1u, "count_stale reports stale entry");
    expectTrue(cooker.cache().estimate_prune_reconcile() == 1u, "estimate_prune_reconcile reports stale entry");
    const std::vector<std::string> stale_sources = cooker.cache().probe_stale_content_sources();
    expectTrue(stale_sources.size() == 1u, "probe_stale_content lists one stale source");
    expectTrue(stale_sources[0] == source, "probe_stale_content reports updated source path");
    expectTrue(cooker.cache().count_invalid_entries() == 0u,
               "count_invalid on structurally valid stale entry returns zero");
    expectTrue(cooker.cache().estimate_prune_all() == 1u, "estimate_prune_all matches prunable count");

    const std::vector<std::string> stale_sources = cooker.cache().probe_stale_content_sources();
    expectTrue(stale_sources.size() == 1u, "probe_stale_content lists one stale source");
    expectTrue(stale_sources[0] == source, "probe_stale_content reports correct source path");

    const fuse::u32 removed = cooker.cache().prune_stale_entries();
    expectTrue(removed == 1u, "prune removes probed stale entry");
    expectTrue(cooker.cache().count_stale_entries() == 0u, "count_stale zero after prune");
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

void testCookCachePreflightEntryGuards() {
    fuse::project::CookCacheEntry invalid_key;
    invalid_key.content_hash = 0;
    invalid_key.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_key.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight zero_key =
        fuse::project::preflight_cook_cache_entry(invalid_key);
    expectTrue(!zero_key.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_key.reason == fuse::project::CookHashRejectReason::InvalidCacheKey,
               "zero content hash preflight reason is InvalidCacheKey");

    fuse::project::CookCacheEntry empty_source;
    empty_source.content_hash = 42;
    empty_source.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path fails cache entry preflight");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry_valid.obj", "# entry valid\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry_valid.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");

    fuse::project::CookCacheEntry shader_entry = valid;
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    shader_entry.source_path = "/tmp/fuse_b79_preflight_shader_missing.obj";
    expectTrue(fuse::project::preflight_cook_cache_entry(shader_entry).ok(),
               "shader cache entry skips source readability preflight");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::InvalidCacheKey)) == "invalid_cache_key",
               "reject reason label for invalid cache key");
}

void testCookHashPreflightCacheEntryGuards() {
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 707;
    valid.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight valid_preflight =
        fuse::project::preflight_cook_cache_entry(valid);
    expectTrue(valid_preflight.ok(), "valid cache entry passes preflight");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    const fuse::project::CookHashPreflight zero_preflight =
        fuse::project::preflight_cook_cache_entry(zero_key);
    expectTrue(!zero_preflight.ok(), "zero cache key fails preflight");
    expectTrue(zero_preflight.reason == fuse::project::CookHashRejectReason::InvalidCacheKey,
               "zero cache key preflight reason is InvalidCacheKey");

    fuse::project::CookCacheEntry empty_source = valid;
    empty_source.source_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path cache entry preflight reason");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::InvalidCacheKey)) == "invalid_cache_key",
               "reject reason label for invalid cache key");
}

void testCookCacheProbeUniqueStaleUpstreamSources() {
    fuse::project::CookCache cache;
    expectTrue(cache.probe_unique_stale_upstream_sources({}).empty(),
               "unique stale upstream probe on empty cache returns empty list");

    fuse::project::CookCacheEntry entry_a;
    entry_a.content_hash = 901;
    entry_a.upstream_hash = 1;
    entry_a.source_path = "/tmp/fuse_b79_unique_upstream_a.obj";
    entry_a.output_path = "/tmp/fuse_b79_unique_upstream_a.fusemesh";
    cache.store(entry_a);

    fuse::project::CookCacheEntry entry_b = entry_a;
    entry_b.content_hash = 902;
    entry_b.source_path = "/tmp/fuse_b79_unique_upstream_b.obj";
    entry_b.output_path = "/tmp/fuse_b79_unique_upstream_b.fusemesh";
    cache.store(entry_b);

    const std::vector<std::pair<std::string, fuse::u64>> stale_pairs = {
        {entry_a.source_path, 99u},
        {entry_a.source_path, 99u},
        {entry_b.source_path, 99u},
    };
    const std::vector<std::string> raw = cache.probe_stale_upstream_sources(stale_pairs);
    expectTrue(raw.size() == 3u, "raw stale upstream probe reports one push per matching entry");

    const std::vector<std::string> unique = cache.probe_unique_stale_upstream_sources(stale_pairs);
    expectTrue(unique.size() == 2u, "unique stale upstream probe deduplicates source paths");
}

void testCookHashTryPreflightAndShouldSkipGuards() {
    fuse::project::CookHashRejectReason reason = fuse::project::CookHashRejectReason::None;

    fuse::project::MeshImportDesc empty_mesh;
    empty_mesh.output_path = "/tmp/fuse_b79_try_preflight.fusemesh";
    expectTrue(!fuse::project::tryPreflightMeshImportHash(empty_mesh, reason),
               "tryPreflightMeshImportHash rejects empty input");
    expectTrue(reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "tryPreflightMeshImportHash empty input reason");
    expectTrue(fuse::project::shouldSkipMeshImportHash(empty_mesh),
               "shouldSkipMeshImportHash true for empty input");

    const std::string source = writeTempFile("/tmp/fuse_b79_try_preflight.obj", "# try preflight\n");
    fuse::project::MeshImportDesc mesh;
    mesh.input_path = source;
    mesh.output_path = "/tmp/fuse_b79_try_preflight.fusemesh";
    expectTrue(fuse::project::tryPreflightMeshImportHash(mesh, reason),
               "tryPreflightMeshImportHash accepts readable mesh");
    expectTrue(!fuse::project::shouldSkipMeshImportHash(mesh),
               "shouldSkipMeshImportHash false for readable mesh");

    fuse::project::TextureImportDesc tex;
    tex.input_path = source;
    tex.output_path = "/tmp/fuse_b79_try_preflight.fusetex";
    expectTrue(fuse::project::tryPreflightTextureImportHash(tex, reason),
               "tryPreflightTextureImportHash accepts readable texture");

    fuse::project::AudioImportDesc audio;
    audio.input_path = source;
    audio.output_path = "/tmp/fuse_b79_try_preflight.fuseaudio";
    expectTrue(fuse::project::tryPreflightAudioImportHash(audio, reason),
               "tryPreflightAudioImportHash accepts readable audio");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_try_preflight_manifest.fusemesh";
    expectTrue(fuse::project::tryPreflightManifestEntryHash(entry, reason),
               "tryPreflightManifestEntryHash accepts readable entry");
    expectTrue(!fuse::project::shouldSkipManifestEntryHash(entry),
               "shouldSkipManifestEntryHash false for readable entry");

    fuse::project::CookCacheEntry cache_entry;
    cache_entry.content_hash = 909;
    cache_entry.source_path = source;
    cache_entry.output_path = "/tmp/fuse_b79_try_preflight_cache.fusemesh";
    cache_entry.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::tryPreflightCookCacheEntry(cache_entry, reason),
               "tryPreflightCookCacheEntry accepts readable cache entry");
    expectTrue(!fuse::project::shouldSkipCookCacheEntry(cache_entry),
               "shouldSkipCookCacheEntry false for readable entry");

    fuse::project::CookCacheEntry invalid_entry = cache_entry;
    invalid_entry.content_hash = 0;
    expectTrue(!fuse::project::tryPreflightCookCacheEntry(invalid_entry, reason),
               "tryPreflightCookCacheEntry rejects zero hash");
    expectTrue(reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "tryPreflightCookCacheEntry zero hash reason");
    expectTrue(fuse::project::shouldSkipCookCacheEntry(invalid_entry),
               "shouldSkipCookCacheEntry true for zero hash");

    const fuse::project::CookHashPreflight entry_preflight =
        fuse::project::preflight_cook_cache_entry(cache_entry);
    expectTrue(entry_preflight.ok(), "preflight_cook_cache_entry ok for valid entry");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_inv.obj"),
               "would_invalidate_source false on empty cache");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_inv.fusemesh"),
               "would_invalidate_output false on empty cache");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_inv.obj", 42u),
               "would_invalidate_stale_content false on empty cache");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_inv.obj", 1u}}),
               "would_invalidate_stale_upstream false on empty cache");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_inv.fusemesh", {}, {}),
               "would_invalidate_downstream false on empty cache");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv_mesh.obj", "# would inv\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_inv_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source true for seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output true for seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false for matching hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true for mismatched hash");

    writeTempFile(source, "# would inv updated\n");
    expectTrue(cooker.cache().would_prune_all(), "would_prune_all true after content change");
}

void testCookCacheProbeStaleUpstreamSourcesUnique() {
    const std::string source = writeTempFile("/tmp/fuse_b79_unique_upstream.obj", "# unique upstream\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_unique_upstream.fusemesh";

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_mesh(desc).ok, "seed cook for unique upstream probe");

    const std::vector<std::pair<std::string, fuse::u64>> stale_pairs = {
        {source, 999u},
        {source, 1000u},
    };
    const std::vector<std::string> duplicated =
        cooker.cache().probe_stale_upstream_sources(stale_pairs);
    expectTrue(duplicated.size() == 2u, "non-deduped upstream probe preserves duplicate entries");

    const std::vector<std::string> unique =
        cooker.cache().probe_stale_upstream_sources_unique(stale_pairs);
    expectTrue(unique.size() == 1u, "deduped upstream probe collapses duplicate sources");
    expectTrue(unique[0] == source, "deduped upstream probe retains source path");
}

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

void testContentHashPreflightGuards() {
    expectTrue(!fuse::project::is_valid_content_hash(0), "zero content hash is invalid");
    expectTrue(fuse::project::is_valid_content_hash(42u), "non-zero content hash is valid");

    const fuse::project::CookContentHashPreflight empty_path =
        fuse::project::preflight_file_content_hash("");
    expectTrue(empty_path.empty_path, "preflight marks empty path");
    expectTrue(!empty_path.can_hash(), "empty path preflight cannot hash");

    const fuse::project::CookContentHashPreflight missing =
        fuse::project::preflight_file_content_hash("/tmp/fuse_b79_preflight_missing.obj");
    expectTrue(missing.missing_file, "preflight marks missing file");
    expectTrue(!missing.can_hash(), "missing file preflight cannot hash");

    const fuse::project::CookContentHashPreflight ok = fuse::project::preflight_file_content_hash(source);
    expectTrue(ok.can_hash(), "readable file preflight can hash");
    expectTrue(fuse::project::is_valid_content_hash(fuse::project::hash_file_content(source)),
               "preflight agrees with hash_file_content on valid path");

    fuse::project::MeshImportDesc mesh;
    mesh.input_path = "";
    mesh.output_path = "/tmp/fuse_b79_preflight_mesh.fusemesh";
    const fuse::project::CookImportHashPreflight mesh_preflight = fuse::project::preflight_mesh_import(mesh);
    expectTrue(mesh_preflight.empty_input_path, "mesh preflight marks empty input");
    expectTrue(!mesh_preflight.can_hash(), "mesh preflight rejects empty input");

    mesh.input_path = source;
    const fuse::project::CookImportHashPreflight mesh_ok = fuse::project::preflight_mesh_import(mesh);
    expectTrue(mesh_ok.can_hash(), "mesh preflight accepts valid descriptor");
    expectTrue(fuse::project::hash_mesh_import(mesh) != 0, "hash agrees with mesh preflight");
}

void testCookCachePreflightAndLookupGuards() {
    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;

    const fuse::project::CookCacheStorePreflight store_preflight =
        fuse::project::preflight_cook_cache_store(invalid);
    expectTrue(store_preflight.zero_content_hash, "store preflight marks zero hash");
    expectTrue(!store_preflight.can_store(), "zero-hash entry cannot store");

    fuse::project::CookCache cache;
    const fuse::project::CookCacheLookupPreflight empty_lookup = cache.preflight_lookup(77u);
    expectTrue(empty_lookup.cache_empty, "preflight lookup marks empty cache");
    expectTrue(!empty_lookup.would_hit(), "empty cache preflight would miss");

    fuse::project::CookCacheEntry valid = invalid;
    valid.content_hash = 808;
    cache.store(valid);
    const fuse::project::CookCacheLookupPreflight hit_lookup = fuse::project::preflight_cook_cache_lookup(cache, 808u);
    expectTrue(hit_lookup.would_hit(), "preflight lookup would hit stored entry");
    expectTrue(cache.preflight_lookup(0).zero_content_hash, "zero-hash lookup preflight guarded");

void testCookCacheInvalidationProbes() {
    const std::string source = writeTempFile("/tmp/fuse_b79_probe_mesh.obj", "# probe mesh\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_probe_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord cooked = cooker.cook_mesh(desc);
    expectTrue(cooked.ok, "seed cook for probe tests ok");

    fuse::project::CookCache& cache = cooker.cache();
    const fuse::u64 invalidations_before = cache.stats().invalidations;

    const fuse::project::CookCacheInvalidationProbe unknown_hash = cache.probe_invalidate(cooked.content_hash + 1u);
    expectTrue(!unknown_hash.would_invalidate(), "unknown hash probe reports no removal");
    expectTrue(cache.stats().invalidations == invalidations_before, "probe does not bump invalidation stats");

    const fuse::project::CookCacheInvalidationProbe known_hash = cache.probe_invalidate(cooked.content_hash);
    expectTrue(known_hash.would_invalidate(), "known hash probe reports removal");
    expectTrue(known_hash.would_invalidate_count == 1u, "known hash probe counts one entry");

    const fuse::project::CookCacheInvalidationProbe source_probe = cache.probe_invalidate_source(source);
    expectTrue(source_probe.would_invalidate_count == 1u, "source probe counts seeded entry");

    const fuse::project::CookCacheInvalidationProbe stale_probe =
        cache.probe_invalidate_stale_content_for_source(source, cooked.content_hash + 1u);
    expectTrue(stale_probe.would_invalidate_count == 1u, "stale-content probe counts mismatched entry");
    expectTrue(cache.probe_invalidate_stale_content_for_source(source, 0u).invalid_args,
               "stale-content probe rejects zero current hash");

    const fuse::project::CookCacheInvalidationProbe output_probe =
        cache.probe_invalidate_output(desc.output_path);
    expectTrue(output_probe.would_invalidate_count == 1u, "output probe counts seeded entry");

    expectTrue(cache.probe_invalidate_source("").invalid_args, "empty source probe guarded");
    expectTrue(!cache.probe_invalidate_downstream_of("", {}, {}).would_invalidate(),
               "empty downstream probe guarded");

void testCookCacheReconcileEstimatorGuards() {
    const fuse::project::CookCacheReconcileEstimate empty_estimate = cache.estimate_reconcile();
    expectTrue(!empty_estimate.needs_reconcile(), "empty cache reconcile estimate is clean");
    expectTrue(empty_estimate.total_prunable() == 0u, "empty cache prunable total is zero");
    expectTrue(!cache.has_invalid_entries(), "empty cache has no invalid entries");
    expectTrue(!cache.has_stale_entries(), "empty cache has no stale entries");
    expectTrue(cache.count_prunable_entries() == 0u, "empty cache prunable count is zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_estimate_stale.obj", "# estimate v1\n");
    desc.output_path = "/tmp/fuse_b79_estimate_stale.fusemesh";

    expectTrue(cooked.ok, "seed cook for reconcile estimate ok");
    expectTrue(!cooker.estimate_cache_reconcile().needs_reconcile(), "fresh cache reconcile estimate is clean");

    writeTempFile(source, "# estimate v2\n");
    const fuse::project::CookCacheReconcileEstimate stale_estimate = cooker.cache().estimate_reconcile();
    expectTrue(stale_estimate.needs_reconcile(), "stale cache reconcile estimate is dirty");
    expectTrue(stale_estimate.stale_entry_count == 1u, "stale reconcile estimate counts one stale entry");
    expectTrue(cooker.cache().count_prunable_entries() == 1u, "count_prunable matches reconcile estimate");
    expectTrue(cooker.cache().has_stale_entries(), "has_stale_entries agrees with estimate");

    const fuse::u32 removed = cooker.cache().prune_all();
    expectTrue(removed == stale_estimate.total_prunable(), "prune_all removes estimated prunable count");
    expectTrue(!cooker.estimate_cache_reconcile().needs_reconcile(), "post-prune reconcile estimate is clean");

void testCookCacheStaleClassificationGuards() {
    invalid.source_path = "/tmp/fuse_b79_classify_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_classify_invalid.fusemesh";
    expectTrue(fuse::project::is_invalid_cook_cache_entry(invalid), "zero-key entry is invalid");
    expectTrue(!fuse::project::is_stale_cook_cache_entry(invalid), "invalid entry is not classified stale");

    const std::string source = writeTempFile("/tmp/fuse_b79_classify_stale.obj", "# classify v1\n");
    desc.output_path = "/tmp/fuse_b79_classify_stale.fusemesh";

    expectTrue(cooked.ok, "seed cook for classification ok");

    fuse::project::CookCacheEntry fresh;
    fresh.content_hash = cooked.content_hash;
    fresh.source_path = source;
    fresh.output_path = desc.output_path;
    fresh.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(!fuse::project::is_stale_cook_cache_entry(fresh), "fresh entry is not stale");

    writeTempFile(source, "# classify v2\n");
    expectTrue(fuse::project::is_stale_cook_cache_entry(fresh), "unchanged record is stale after source edit");

void testCookCacheKeyPreflight() {
    fuse::project::CookCacheKeyRejectReason reason = fuse::project::CookCacheKeyRejectReason::None;

    expectTrue(!fuse::project::preflight_cook_cache_key(0, 0, &reason),
               "zero source fails cache key preflight");
    expectTrue(reason == fuse::project::CookCacheKeyRejectReason::ZeroSource,
               "zero source reports zero_source reject reason");

    expectTrue(fuse::project::preflight_cook_cache_key(99u, 0, &reason),
               "valid source-only fold passes preflight");
    expectTrue(reason == fuse::project::CookCacheKeyRejectReason::None,
               "valid fold reports none reject reason");

    expectTrue(fuse::project::preflight_cook_cache_key(99u, 42u, &reason),
               "valid combined fold passes preflight");
    expectTrue(std::string(fuse::project::cookCacheKeyRejectReasonLabel(
                   fuse::project::CookCacheKeyRejectReason::ZeroSource)) == "zero_source",
               "cache key reject reason label is stable");

void testCookHashPreflight() {
    fuse::project::CookHashRejectReason reason = fuse::project::CookHashRejectReason::None;

    expectTrue(!fuse::project::preflight_hash_file_content("", nullptr, &reason),
               "empty path fails file hash preflight");
    expectTrue(reason == fuse::project::CookHashRejectReason::EmptyPath,
               "empty path reports empty_path reject reason");

    expectTrue(!fuse::project::preflight_hash_file_content("/tmp/fuse_b79_missing_preflight.obj", nullptr, &reason),
               "missing file fails file hash preflight");
    expectTrue(reason == fuse::project::CookHashRejectReason::Unreadable,
               "missing file reports unreadable reject reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_hash.obj", "# preflight hash\n");
    fuse::u64 hash = 0;
    expectTrue(fuse::project::preflight_hash_file_content(source, &hash, &reason),
               "readable file passes file hash preflight");
    expectTrue(hash == fuse::project::hash_file_content(source),
               "preflight file hash matches hash_file_content");
    expectTrue(reason == fuse::project::CookHashRejectReason::None,
               "readable file reports none reject reason");

    desc.output_path = "/tmp/fuse_b79_preflight_hash.fusemesh";
    fuse::u64 mesh_hash = 0;
    expectTrue(fuse::project::preflight_mesh_import_hash(desc, &mesh_hash, &reason),
               "valid mesh desc passes import hash preflight");
    expectTrue(mesh_hash == fuse::project::hash_mesh_import(desc),
               "preflight mesh hash matches hash_mesh_import");

void testCookCachePreflightLookupAndStore() {
    fuse::project::CookCache::LookupRejectReason lookup_reason =
        fuse::project::CookCache::LookupRejectReason::None;

    expectTrue(cache.preflight_lookup(0, &lookup_reason) == fuse::project::CookCacheLookup::Miss,
               "zero key preflight misses");
    expectTrue(lookup_reason == fuse::project::CookCache::LookupRejectReason::ZeroKey,
               "zero key reports zero_key reject reason");
    expectTrue(cache.stats().misses == 0u, "preflight lookup does not bump miss stats");

    expectTrue(cache.preflight_lookup(77u, &lookup_reason) == fuse::project::CookCacheLookup::Miss,
               "valid key preflight misses on empty cache");
    expectTrue(lookup_reason == fuse::project::CookCache::LookupRejectReason::EmptyCache,
               "empty cache reports empty_cache reject reason");

    entry.content_hash = 201;
    entry.source_path = "/tmp/fuse_b79_preflight_store.obj";
    entry.output_path = "/tmp/fuse_b79_preflight_store.fusemesh";
    fuse::project::CookCache::StoreRejectReason store_reason =
        fuse::project::CookCache::StoreRejectReason::None;
    expectTrue(cache.preflight_store(entry, &store_reason), "valid entry passes store preflight");
    expectTrue(store_reason == fuse::project::CookCache::StoreRejectReason::None,
               "valid entry reports none store reject reason");

    fuse::project::CookCacheEntry invalid = entry;
    expectTrue(!cache.preflight_store(invalid, &store_reason), "invalid entry fails store preflight");
    expectTrue(store_reason == fuse::project::CookCache::StoreRejectReason::InvalidEntry,
               "invalid entry reports invalid_entry store reject reason");

    expectTrue(cache.preflight_lookup(201u, &lookup_reason) == fuse::project::CookCacheLookup::Hit,
               "stored key preflight hits");
    expectTrue(lookup_reason == fuse::project::CookCache::LookupRejectReason::None,
               "hit reports none lookup reject reason");

    expectTrue(cache.preflight_lookup(202u, &lookup_reason) == fuse::project::CookCacheLookup::Miss,
               "unknown key preflight misses");
    expectTrue(lookup_reason == fuse::project::CookCache::LookupRejectReason::NotFound,
               "unknown key reports not_found reject reason");

void testCookCacheCountProbes() {
    const std::string source = writeTempFile("/tmp/fuse_b79_count_probe.obj", "# count probe v1\n");
    desc.output_path = "/tmp/fuse_b79_count_probe.fusemesh";

    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for count probes ok");

    expectTrue(cooker.cache().count_stale_content_for_source(source, seeded.content_hash) == 0u,
               "matching hash reports zero stale content");
    expectTrue(cooker.cache().count_stale_content_for_source(source, seeded.content_hash + 1u) == 1u,
               "mismatched hash reports one stale content entry");
    expectTrue(cooker.cache().count_source_entries(source) == 1u,
               "one entry matches source path");

    writeTempFile(source, "# count probe v2\n");
    const fuse::u32 prunable_before = cooker.cache().count_prunable_entries();
    expectTrue(prunable_before == 1u, "stale entry increments prunable count");
    expectTrue(cooker.cache().has_prunable_entries(), "has_prunable agrees with positive count");

    const fuse::u32 pruned = cooker.cache().prune_all();
    expectTrue(pruned == prunable_before, "prune_all removes counted prunable entries");
    expectTrue(cooker.cache().count_prunable_entries() == 0u, "cache clean after prune");

void testCookCacheStaleUpstreamCountProbe() {
    const std::string source = writeTempFile("/tmp/fuse_b79_upstream_count.obj", "# upstream count\n");

    entry.content_hash = 301;
    entry.upstream_hash = 10;
    entry.output_path = "/tmp/fuse_b79_upstream_count.fusemesh";

    expectTrue(cache.count_stale_upstream_entries({{source, 10u}}) == 0u,
               "matching upstream hash reports zero stale entries");
    expectTrue(cache.count_stale_upstream_entries({{source, 11u}}) == 1u,
               "mismatched upstream hash reports one stale entry");
    expectTrue(cache.count_stale_upstream_entries({}) == 0u,
               "empty upstream list reports zero stale entries");

    const fuse::project::CookCacheKeyPreflight zero =
        fuse::project::preflight_cook_cache_key(0, 0);
    expectTrue(zero.zero_source, "preflight marks zero source");
    expectTrue(zero.zero_combined, "preflight marks zero combined key");
    expectTrue(!zero.cacheable, "zero fold is not cacheable");
    expectTrue(!zero.can_cache(), "can_cache rejects zero fold");

    const fuse::project::CookCacheKeyPreflight source_only =
        fuse::project::preflight_cook_cache_key(99u, 0);
    expectTrue(!source_only.zero_source, "valid source clears zero_source");
    expectTrue(!source_only.zero_combined, "valid source yields non-zero combined key");
    expectTrue(source_only.cacheable, "valid source-only fold is cacheable");

    const fuse::project::CookCacheKeyPreflight combined =
        fuse::project::preflight_cook_cache_key(99u, 42u);
    expectTrue(combined.cacheable, "valid combined fold is cacheable");
    expectTrue(combined.can_cache() == fuse::project::is_cacheable_cook_cache_key(99u, 42u),
               "can_cache mirrors is_cacheable_cook_cache_key");

void testCookCacheEntryPreflight() {
    invalid.source_path = "/tmp/fuse_b79_preflight_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_preflight_invalid.fusemesh";

    const fuse::project::CookCacheEntryPreflight invalid_preflight =
        fuse::project::preflight_cache_entry(invalid);
    expectTrue(invalid_preflight.zero_key, "preflight marks zero key");
    expectTrue(!invalid_preflight.structurally_valid, "invalid entry fails structural preflight");
    expectTrue(invalid_preflight.is_prunable(), "invalid entry is prunable");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_valid.obj", "# preflight valid\n");
    desc.output_path = "/tmp/fuse_b79_preflight_valid.fusemesh";

    expectTrue(cooked.ok, "seed cook for entry preflight ok");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = cooked.content_hash;
    valid.source_path = source;
    valid.output_path = desc.output_path;
    valid.kind = fuse::project::CookAssetKind::Mesh;

    const fuse::project::CookCacheEntryPreflight fresh_preflight =
        fuse::project::preflight_cache_entry(valid);
    expectTrue(fresh_preflight.structurally_valid, "fresh entry passes structural preflight");
    expectTrue(!fresh_preflight.zero_key, "fresh entry has non-zero key");
    expectTrue(!fresh_preflight.empty_path, "fresh entry has valid paths");
    expectTrue(!fresh_preflight.source_missing, "fresh entry source exists on disk");
    expectTrue(!fresh_preflight.stale_content, "fresh entry is not stale");
    expectTrue(!fresh_preflight.is_prunable(), "fresh entry is not prunable");
    expectTrue(fresh_preflight.can_store(), "fresh entry can be stored");

    writeTempFile(source, "# preflight stale\n");
    const fuse::project::CookCacheEntryPreflight stale_preflight =
    expectTrue(stale_preflight.stale_content, "content change marks entry stale");
    expectTrue(stale_preflight.is_prunable(), "stale entry is prunable");

    expectTrue(!fuse::project::is_valid_cook_cache_entry_on_disk(valid),
               "stub cook does not create output artifact on disk");

void testCookCachePruneEstimateProbes() {
    const std::string valid_source = writeTempFile("/tmp/fuse_b79_estimate_valid.obj", "# estimate valid\n");
    const std::string stale_source = writeTempFile("/tmp/fuse_b79_estimate_stale.obj", "# estimate v1\n");

    fuse::project::MeshImportDesc valid_desc;
    valid_desc.input_path = valid_source;
    valid_desc.output_path = "/tmp/fuse_b79_estimate_valid.fusemesh";

    fuse::project::MeshImportDesc stale_desc;
    stale_desc.input_path = stale_source;
    stale_desc.output_path = "/tmp/fuse_b79_estimate_stale.fusemesh";

    const fuse::project::CookRecord valid_cook = cooker.cook_mesh(valid_desc);
    const fuse::project::CookRecord stale_cook = cooker.cook_mesh(stale_desc);
    expectTrue(valid_cook.ok && stale_cook.ok, "seed entries for prune estimate");

    expectTrue(cooker.cache().count_prunable_entries() == 0u, "fresh cache has zero prunable count");
    const fuse::project::CookCachePruneEstimate fresh_estimate = cooker.cache().estimate_prune_all();
    expectTrue(!fresh_estimate.would_prune(), "fresh cache would not prune");
    expectTrue(fresh_estimate.total() == 0u, "fresh estimate total is zero");

    writeTempFile(stale_source, "# estimate v2\n");
    expectTrue(cooker.cache().count_prunable_entries() == 1u,
               "count_prunable matches has_prunable for stale entry");
    expectTrue(cooker.cache().has_prunable_entries(), "has_prunable agrees with count");

    const fuse::project::CookCachePruneEstimate stale_estimate = cooker.cache().estimate_prune_all();
    expectTrue(stale_estimate.would_prune(), "stale cache would prune");
    expectTrue(stale_estimate.stale_entries == 1u, "estimate counts one stale entry");
    expectTrue(stale_estimate.invalid_entries == 0u, "estimate has no invalid entries");
    expectTrue(stale_estimate.total() == cooker.cache().count_prunable_entries(),
               "estimate total matches count_prunable");

    expectTrue(removed == stale_estimate.total(), "prune_all removes estimated count");

void testCookCacheStaleUpstreamProbes() {

    fuse::project::CookManifest manifest;


    expectTrue(batch.ok, "manifest cook seeds upstream probe cache");

    expectTrue(cooker.estimate_stale_dependency_invalidations(manifest) == 0u,
               "estimate is zero before upstream hash change");

    const fuse::u32 estimated = cooker.estimate_stale_dependency_invalidations(manifest);
    expectTrue(estimated >= 1u, "estimate reports stale upstream invalidations");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed == estimated, "estimate matches actual stale dependency invalidation count");
               "estimate is zero after reconcile");

void testCookContentHashPreflightGuards() {
    expectTrue(fuse::project::should_skip_hash_file_content(""), "empty path skips file hash preflight");
    const fuse::project::CookHashPreflight empty_preflight = fuse::project::preflight_hash_file_content("");
    expectTrue(empty_preflight.empty_path, "empty path preflight marks empty_path");
    expectTrue(empty_preflight.should_skip(), "empty path preflight should skip");

    expectTrue(fuse::project::should_skip_hash_file_content("/tmp/fuse_b79_preflight_missing.obj"),
               "missing file skips hash preflight");
    const fuse::project::CookHashPreflight missing_preflight =
        fuse::project::preflight_hash_file_content("/tmp/fuse_b79_preflight_missing.obj");
    expectTrue(missing_preflight.missing_file, "missing file preflight marks missing_file");
    expectTrue(!missing_preflight.empty_path, "missing file preflight clears empty_path");
    expectTrue(missing_preflight.should_skip(), "missing file preflight should skip");

    const fuse::project::CookHashPreflight valid_preflight =
        fuse::project::preflight_hash_file_content(source);
    expectTrue(valid_preflight.can_hash(), "existing file passes hash preflight");
    expectTrue(!valid_preflight.should_skip(), "valid file preflight does not skip");
    expectTrue(fuse::project::hash_file_content(source) != 0,
               "valid preflight path still yields non-zero hash");

    expectTrue(fuse::project::preflight_hash_mesh_import(desc).can_hash(),
               "valid mesh import passes hash preflight");
    desc.output_path = "";
    expectTrue(fuse::project::preflight_hash_mesh_import(desc).should_skip(),
               "empty output path fails mesh hash preflight");

    const fuse::project::CookCacheKeyPreflight key_preflight =
        fuse::project::preflight_combine_cook_cache_key(0, 42u);
    expectTrue(key_preflight.zero_source_hash, "zero source hash preflight flagged");
    expectTrue(fuse::project::should_skip_combine_cook_cache_key(0, 42u),
               "zero source combine preflight skips");
    expectTrue(fuse::project::preflight_combine_cook_cache_key(99u, 0).can_combine(),
               "valid source with zero upstream can combine");


    const fuse::project::CookCacheInvalidationProbe empty_probe = cache.probe_invalidate(42u);
    expectTrue(empty_probe.empty_cache, "probe on empty cache marks empty_cache");
    expectTrue(empty_probe.should_skip(), "probe on empty cache should skip");

    valid.content_hash = 801;
    valid.source_path = "/tmp/fuse_b79_probe_source.obj";
    valid.output_path = "/tmp/fuse_b79_probe_source.fusemesh";
    expectTrue(cache.entry_count() == 1u, "probe setup stores valid entry");

    const fuse::project::CookCacheInvalidationProbe zero_probe = cache.probe_invalidate(0);
    expectTrue(zero_probe.zero_hash, "zero hash probe flagged");
    expectTrue(zero_probe.should_skip(), "zero hash probe should skip");

    const fuse::project::CookCacheInvalidationProbe known_probe = cache.probe_invalidate(801u);
    expectTrue(known_probe.would_invalidate(), "known hash probe would invalidate");
    expectTrue(known_probe.affected_entries == 1u, "known hash probe counts one entry");
    expectTrue(!known_probe.empty_cache, "populated cache probe clears empty_cache");

    const fuse::project::CookCacheInvalidationProbe unknown_probe = cache.probe_invalidate(802u);
    expectTrue(unknown_probe.should_skip(), "unknown hash probe should skip");

    const fuse::project::CookCacheInvalidationProbe source_probe =
        cache.probe_invalidate_source(valid.source_path);
    expectTrue(source_probe.would_invalidate(), "source probe would invalidate");
    expectTrue(source_probe.affected_entries == 1u, "source probe counts one entry");

        cache.probe_invalidate_output(valid.output_path);
    expectTrue(output_probe.would_invalidate(), "output probe would invalidate");

    expectTrue(cache.probe_invalidate_source("").empty_path, "empty source path probe flagged");
    expectTrue(cache.probe_invalidate_stale_content_for_source(valid.source_path, 0).zero_hash,
               "zero current hash probe flagged");
    expectTrue(cache.probe_invalidate_stale_content_for_source(valid.source_path, 802u).would_invalidate(),
               "mismatched current hash probe would invalidate");
    expectTrue(cache.probe_invalidate_stale_content_for_source(valid.source_path, 801u).should_skip(),
               "matching current hash probe should skip");

    expectTrue(cache.invalidate(801u), "actual invalidate still works after probes");
    expectTrue(cache.stats().invalidations == invalidations_before + 1u,
               "probes do not mutate invalidation stats");

void testCookCacheReconcileEstimators() {

    fuse::project::CookCacheReconcileEstimate empty_estimate = cache.estimate_prune_all();
    expectTrue(empty_estimate.empty_cache, "empty cache reconcile estimate marks empty_cache");
    expectTrue(empty_estimate.should_skip(), "empty cache reconcile estimate should skip");

    invalid.source_path = "/tmp/fuse_b79_estimate_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_estimate_invalid.fusemesh";
    cache.store(invalid);
    expectTrue(cache.entry_count() == 0u, "invalid entry not stored for reconcile estimate setup");


    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "seed cook for reconcile estimate ok");
    expectTrue(!cooker.cache().estimate_prune_all().would_reconcile(),
               "fresh cook cache reconcile estimate should skip");

    const fuse::project::CookCacheReconcileEstimate stale_estimate = cooker.cache().estimate_prune_stale_entries();
    expectTrue(stale_estimate.would_reconcile(), "stale content reconcile estimate would reconcile");
    expectTrue(stale_estimate.stale_entries == 1u, "stale reconcile estimate counts one stale entry");
    expectTrue(stale_estimate.total_removable == 1u, "stale reconcile estimate total matches stale count");

    const fuse::project::CookCacheReconcileEstimate all_estimate = cooker.cache().estimate_prune_all();
    expectTrue(all_estimate.would_reconcile(), "prune_all estimate would reconcile");
    expectTrue(all_estimate.stale_entries == 1u, "prune_all estimate counts stale entry");
    expectTrue(all_estimate.invalid_entries == 0u, "prune_all estimate has no invalid entries");

    expectTrue(removed == all_estimate.total_removable,
               "reconcile estimate matches prune_all removal count");
    expectTrue(cooker.cache().estimate_prune_all().should_skip(),
               "clean cache reconcile estimate should skip after prune");

void testCookHashPreflightGuards() {
    const fuse::project::CookHashPreflight empty_path = fuse::project::preflight_hash_file_content("");
    expectTrue(empty_path.empty_path, "preflight marks empty file path");
    expectTrue(!empty_path.can_hash(), "empty file path cannot hash");

    const fuse::project::CookHashPreflight missing =
    expectTrue(missing.source_missing, "preflight marks missing source");
    expectTrue(!missing.can_hash(), "missing source cannot hash");

    const fuse::project::CookHashPreflight ok = fuse::project::preflight_hash_file_content(source);
    expectTrue(ok.can_hash(), "existing source passes preflight");
    expectTrue(fuse::project::hash_file_content(source) != 0, "preflight ok path still hashes");

    const fuse::project::CookHashPreflight mesh_empty_input = fuse::project::preflight_mesh_import(mesh);
    expectTrue(mesh_empty_input.empty_input_path, "mesh preflight marks empty input");
    expectTrue(!mesh_empty_input.can_hash(), "mesh with empty input cannot hash");

    mesh.output_path = "";
    const fuse::project::CookHashPreflight mesh_empty_output = fuse::project::preflight_mesh_import(mesh);
    expectTrue(mesh_empty_output.empty_output_path, "mesh preflight marks empty output");
    expectTrue(!mesh_empty_output.can_hash(), "mesh with empty output cannot hash");

    expectTrue(fuse::project::preflight_mesh_import(mesh).can_hash(), "valid mesh passes preflight");

    expectTrue(cache.probe_stale_content_invalidation("/tmp/fuse_b79_probe.obj", 42u) == 0u,
               "stale-content probe on empty cache returns zero");
    expectTrue(cache.probe_stale_upstream_invalidation({}).empty(),
               "upstream probe on empty cache returns empty list");
    expectTrue(cache.probe_stale_upstream_invalidation_count({}) == 0u,
               "upstream probe count on empty cache returns zero");
    expectTrue(cache.probe_invalidate_downstream_of("/tmp/fuse_b79_probe.fusemesh", {}, {}) == 0u,
               "downstream probe on empty cache returns zero");

    entry.content_hash = 801;
    entry.source_path = "/tmp/fuse_b79_probe_source.obj";
    entry.output_path = "/tmp/fuse_b79_probe_out.fusemesh";
    expectTrue(cache.entry_count() == 1u, "probe setup stores entry");

    expectTrue(cache.probe_stale_content_invalidation(entry.source_path, entry.content_hash) == 0u,
               "matching content hash probe returns zero");
    expectTrue(cache.probe_stale_content_invalidation(entry.source_path, entry.content_hash + 1u) == 1u,
               "mismatched content hash probe counts one entry");
    expectTrue(cache.probe_stale_content_invalidation("", entry.content_hash + 1u) == 0u,
               "empty source path stale-content probe is guarded");

    const std::vector<std::pair<std::string, fuse::u64>> matching_upstream = {{entry.source_path, 10u}};
    expectTrue(cache.probe_stale_upstream_invalidation(matching_upstream).empty(),
               "matching upstream probe returns empty list");
    expectTrue(cache.probe_stale_upstream_invalidation_count(matching_upstream) == 0u,
               "matching upstream probe count is zero");

    const std::vector<std::pair<std::string, fuse::u64>> stale_upstream = {{entry.source_path, 99u}};
    expectTrue(cache.probe_stale_upstream_invalidation(stale_upstream).size() == 1u,
               "stale upstream probe lists affected source");
    expectTrue(cache.probe_stale_upstream_invalidation_count(stale_upstream) == 1u,
               "stale upstream probe count matches list size");

    expectTrue(cache.invalidate_stale_upstream_hashes(stale_upstream).size() == 1u,
               "actual upstream invalidation still removes probed entry");
               "probe path does not mutate stats until invalidation runs");

    expectTrue(empty_estimate.total_removable() == 0u, "empty cache reconcile estimate is zero");
    expectTrue(!empty_estimate.would_change(), "empty cache reconcile would not change");
    expectTrue(cache.estimate_prune_all() == 0u, "empty cache prune_all estimate is zero");

    fuse::project::CookCacheEntry shader_entry;
    shader_entry.content_hash = 901;
    shader_entry.source_path = "/tmp/fuse_b79_estimate_shader.obj";
    shader_entry.output_path = "/tmp/fuse_b79_estimate_shader.fuseshader";
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    cache.store(shader_entry);
    expectTrue(cache.estimate_prune_invalid_entries() == 0u, "valid shader entry is not invalid");
    expectTrue(cache.estimate_prune_stale_entries() == 1u, "shader entry estimated as stale");
    expectTrue(cache.estimate_prune_all() == 1u, "prune_all estimate counts shader stale entry");

    const fuse::project::CookCacheReconcileEstimate shader_estimate = cache.estimate_reconcile();
    expectTrue(shader_estimate.stale_count == 1u, "reconcile estimate counts stale shader entry");
    expectTrue(shader_estimate.would_change(), "shader cache reconcile would change");

    const fuse::u32 prune_estimate = cache.estimate_prune_all();
    expectTrue(removed == prune_estimate, "prune_all matches prior estimate");
    expectTrue(cache.stats().invalidations == invalidations_before + removed,
               "prune after estimate bumps invalidation stats");
    expectTrue(cache.estimate_reconcile().total_removable() == 0u,
               "post-prune reconcile estimate is zero");

void testCookCacheWouldInvalidateProbes() {
               "would_invalidate_source on empty cache is false");
               "would_invalidate_output on empty cache is false");
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(cache.count_prune_all() == 0u, "count_prune_all on empty cache returns zero");



    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content with mismatched hash is true");
    expectTrue(cooker.cache().count_prune_all() == 0u, "fresh entry count_prune_all is zero");

    expectTrue(cooker.cache().count_prune_all() == 1u, "count_prune_all reports stale entry");
    expectTrue(cooker.cache().count_prune_all() == cooker.cache().prune_all(),
               "count_prune_all matches prune_all removal count");
    expectTrue(cooker.cache().count_prune_all() == 0u, "count_prune_all zero after prune");

void testCookCacheStaleUpstreamProbeParity() {
    const std::string source = writeTempFile("/tmp/fuse_b79_upstream_probe.obj", "# upstream probe\n");
    entry.upstream_hash = 100;
    entry.output_path = "/tmp/fuse_b79_upstream_probe.fusemesh";

    expectTrue(cache.entry_count() == 1u, "upstream probe entry stored");

    const std::vector<std::pair<std::string, fuse::u64>> fresh_upstream = {{source, 100u}};
    expectTrue(cache.count_stale_upstream_hashes(fresh_upstream) == 0u,
               "matching upstream hash is not stale");
    expectTrue(cache.probe_stale_upstream_sources(fresh_upstream).empty(),
               "probe_stale_upstream empty when upstream matches");

    const std::vector<std::pair<std::string, fuse::u64>> stale_upstream = {{source, 200u}};
    expectTrue(cache.count_stale_upstream_hashes(stale_upstream) == 1u,
               "mismatched upstream hash counts as stale");
    const std::vector<std::string> probed = cache.probe_stale_upstream_sources(stale_upstream);
    expectTrue(probed.size() == 1u && probed[0] == source, "probe_stale_upstream reports stale source");

    const fuse::u32 removed = static_cast<fuse::u32>(cache.invalidate_stale_upstream_hashes(stale_upstream).size());
    expectTrue(removed == 1u, "invalidate_stale_upstream removes probed entry");
    expectTrue(cache.empty(), "cache empty after upstream invalidation");
    expectTrue(cooker.cache().count_stale_entries() == 0u, "count_stale zero after prune");

void testCookCacheUniqueStaleUpstreamProbe() {
    const std::string source = writeTempFile("/tmp/fuse_b79_unique_probe.obj", "# unique probe\n");

    fuse::project::CookCacheEntry first;
    first.content_hash = 801;
    first.upstream_hash = 11;
    first.source_path = source;
    first.output_path = "/tmp/fuse_b79_unique_probe_a.fusemesh";
    cache.store(first);

    fuse::project::CookCacheEntry second = first;
    second.content_hash = 802;
    second.output_path = "/tmp/fuse_b79_unique_probe_b.fusemesh";
    cache.store(second);

    const std::vector<std::pair<std::string, fuse::u64>> stale_upstream = {
        {source, 99u},
    };
    const std::vector<std::string> stale_sources = cache.probe_stale_upstream_sources(stale_upstream);
    const std::vector<std::string> unique_sources = cache.probe_unique_stale_upstream_sources(stale_upstream);
    expectTrue(stale_sources.size() == 4u, "stale upstream probe reports one hit per duplicate pair");
    expectTrue(unique_sources.size() == 1u, "unique stale upstream probe dedupes source path");
    expectTrue(unique_sources[0] == source, "unique stale upstream probe preserves source path");

void testCookCacheIncrementalInvalidationProbes() {
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_inc_probe.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_inc_probe.fusemesh"),
    expectTrue(cache.count_stale_entries() == 0u, "count_stale_entries on empty cache is zero");
    expectTrue(cache.estimate_prune_removals() == 0u, "estimate_prune_removals on empty cache is zero");
    expectTrue(cache.probe_stale_content_sources().empty(),
               "probe_stale_content_sources on empty cache is empty");

    const std::string source = writeTempFile("/tmp/fuse_b79_inc_probe.obj", "# inc probe v1\n");
    desc.output_path = "/tmp/fuse_b79_inc_probe.fusemesh";

    expectTrue(seeded.ok, "seed cook for incremental probes ok");
    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path probe is false");
    expectTrue(cooker.cache().count_stale_entries() == 0u, "fresh entry is not stale");

    writeTempFile(source, "# inc probe v2\n");
    expectTrue(cooker.cache().count_stale_entries() == 1u, "count_stale_entries reports stale entry");
    expectTrue(cooker.cache().estimate_prune_removals() == 1u,
               "estimate_prune_removals matches stale entry count");
    expectTrue(stale_sources.size() == 1u, "probe_stale_content_sources finds one source");
    expectTrue(stale_sources[0] == source, "probe_stale_content_sources returns stale source path");

    expectTrue(removed == 1u, "prune_all removes estimated stale entry");
    expectTrue(cooker.cache().estimate_prune_removals() == 0u,
               "estimate_prune_removals zero after prune_all");

    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_inv.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_inv.fusemesh"),
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_inv.obj", 42u),
               "probe_stale_content_sources on empty cache returns empty list");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv_mesh.obj", "# would inv\n");
    desc.output_path = "/tmp/fuse_b79_would_inv_mesh.fusemesh";



    writeTempFile(source, "# would inv updated\n");
    expectTrue(stale_sources.size() == 1u, "probe_stale_content_sources finds one stale source");
    expectTrue(stale_sources[0] == source, "probe_stale_content_sources returns matching source path");

void testCookCacheLookupStorePreflights() {

    const fuse::project::CookCacheLookupPreflight zero_lookup =
        fuse::project::preflight_cook_cache_lookup(cache, 0);
    expectTrue(zero_lookup.should_skip(), "zero-key lookup preflight skips");
    expectTrue(!zero_lookup.can_lookup(), "zero-key lookup preflight cannot lookup");

    const fuse::project::CookCacheLookupPreflight empty_lookup =
        fuse::project::preflight_cook_cache_lookup(cache, 42u);
    expectTrue(empty_lookup.can_lookup(), "valid hash lookup preflight can lookup");
    expectTrue(empty_lookup.empty_cache, "empty cache lookup preflight reports empty cache");
    expectTrue(empty_lookup.would_miss(), "valid hash on empty cache would miss");

    const fuse::project::CookCacheStorePreflight valid_store =
        fuse::project::preflight_cook_cache_store(entry);
    expectTrue(valid_store.can_store(), "valid entry store preflight passes");

    entry.content_hash = 0;
    expectTrue(fuse::project::preflight_cook_cache_store(entry).should_skip(),
               "zero-key store preflight skips");
    entry.source_path = "";
               "empty source store preflight skips");

    const fuse::project::CookCacheLookupPreflight hit_lookup =
        fuse::project::preflight_cook_cache_lookup(cache, 201u);
    expectTrue(hit_lookup.would_hit(), "seeded hash lookup preflight would hit");
    expectTrue(!hit_lookup.would_miss(), "seeded hash lookup preflight does not miss");

void testCookFnvInputPreflight() {
    const fuse::project::CookFnvInputPreflight null_preflight =
        fuse::project::preflight_fnv1a64_input(nullptr, 4u);
    expectTrue(null_preflight.should_skip(), "null data with size fails FNV preflight");
    expectTrue(!null_preflight.can_hash(), "null data with size cannot hash");

    const fuse::project::CookFnvInputPreflight empty_preflight =
        fuse::project::preflight_fnv1a64_input(nullptr, 0);
    expectTrue(empty_preflight.can_hash(), "null data with zero size passes FNV preflight");

void testCookCachePruneEstimate() {
    const fuse::project::CookCachePruneEstimate empty_estimate = cache.estimate_prune_removals();
    expectTrue(!empty_estimate.would_prune(), "empty cache prune estimate is zero");
    expectTrue(empty_estimate.total_entries() == 0u, "empty cache prune total is zero");

    invalid.source_path = "/tmp/fuse_b79_prune_est_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_prune_est_invalid.fusemesh";
    expectTrue(cache.estimate_prune_removals().total_entries() == 0u,
               "rejected invalid store does not affect prune estimate");

    const std::string source = writeTempFile("/tmp/fuse_b79_prune_est_stale.obj", "# prune est v1\n");
    desc.output_path = "/tmp/fuse_b79_prune_est_stale.fusemesh";

    expectTrue(seeded.ok, "seed cook for prune estimate ok");

    const fuse::project::CookCachePruneEstimate fresh_estimate = cooker.cache().estimate_prune_removals();
    expectTrue(!fresh_estimate.would_prune(), "fresh cook prune estimate is zero");

    writeTempFile(source, "# prune est v2\n");
    const fuse::project::CookCachePruneEstimate stale_estimate = cooker.cache().estimate_prune_removals();
    expectTrue(stale_estimate.would_prune(), "stale content prune estimate is non-zero");
    expectTrue(stale_estimate.stale_entries == 1u, "stale content counted in prune estimate");
    expectTrue(stale_estimate.invalid_entries == 0u, "valid stale entry is not invalid");
    expectTrue(stale_estimate.total_entries() == cooker.cache().count_prunable_entries(),
               "prune estimate total matches count_prunable");

void testCookCacheProbeAliases() {
    const std::string source = writeTempFile("/tmp/fuse_b79_probe_alias.obj", "# probe alias\n");
    desc.output_path = "/tmp/fuse_b79_probe_alias.fusemesh";

    expectTrue(seeded.ok, "seed cook for probe aliases ok");

    expectTrue(cooker.cache().probe_invalidate_source(source) == cooker.cache().count_by_source(source),
               "probe_invalidate_source matches count_by_source");
    expectTrue(cooker.cache().probe_stale_content_for_source(source, seeded.content_hash + 1u) ==
                   cooker.cache().count_stale_content_for_source(source, seeded.content_hash + 1u),
               "probe_stale_content matches count_stale_content");

    const std::vector<std::pair<std::string, fuse::u64>> upstream = {{source, seeded.content_hash + 1u}};
    expectTrue(cooker.cache().probe_stale_upstream_hash_entries(upstream) ==
                   cooker.cache().count_stale_upstream_hashes(upstream),
               "probe_stale_upstream_hash_entries matches count_stale_upstream_hashes");
    expectTrue(!cooker.cache().probe_stale_upstream_hashes(upstream).empty(),
               "probe_stale_upstream_hashes returns stale source path");

void testCookHashPreflightDeepenGuards() {
    const fuse::project::CookHashPreflight null_bytes = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_bytes.ok(), "null data with non-zero size fails FNV preflight");
               "null data FNV preflight reason");

    const fuse::u8 byte = 0x2a;
    expectTrue(fuse::project::preflight_fnv1a64_bytes(&byte, 1u).ok(),
               "valid FNV preflight passes for non-null data");

    const fuse::project::CookHashPreflight cacheable =
        fuse::project::preflight_cacheable_cook_key(99u, 42u);
    expectTrue(cacheable.ok(), "valid combined key passes cacheable preflight");
    expectTrue(fuse::project::preflight_cacheable_cook_key(99u, 0u).ok(),
               "source-only fold passes cacheable preflight");

    const fuse::project::CookHashPreflight zero_source =
        fuse::project::preflight_cacheable_cook_key(0, 42u);
    expectTrue(!zero_source.ok(), "zero source fails cacheable preflight");
    expectTrue(zero_source.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero source cacheable preflight reason");

    const std::string tex_source = writeTempFile("/tmp/fuse_b79_preflight_tex.png", "# tex\n");
    tex.input_path = tex_source;
    expectTrue(fuse::project::preflight_texture_import_hash(tex).ok(),
               "readable texture import passes hash preflight");
    expectTrue(fuse::project::hash_texture_import(tex) != 0, "texture preflight success implies non-zero hash");

    const std::string audio_source = writeTempFile("/tmp/fuse_b79_preflight_audio.wav", "# audio\n");
    audio.input_path = audio_source;
    expectTrue(fuse::project::preflight_audio_import_hash(audio).ok(),
               "readable audio import passes hash preflight");

    const std::string manifest_source = writeTempFile("/tmp/fuse_b79_preflight_manifest.obj", "# manifest\n");
    entry.source_path = manifest_source;
    expectTrue(fuse::project::preflight_manifest_entry_hash(entry).ok(),
               "readable manifest entry passes hash preflight");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::NonCacheableCombinedKey)) == "non_cacheable_combined_key",
               "reject reason label for non-cacheable combined key");

void testCookCacheDeepenInvalidationProbes() {
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_deepen_probe.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_deepen_probe.fusemesh"),
    expectTrue(cache.count_stale_entries() == 0u, "count_stale_entries on empty cache returns zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_deepen_stale.obj", "# deepen stale v1\n");
    desc.output_path = "/tmp/fuse_b79_deepen_stale.fusemesh";

    expectTrue(seeded.ok, "seed cook for deepen probes ok");

    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path would_invalidate is false");
    expectTrue(cooker.cache().count_prune_all() == 0u, "fresh cache count_prune_all is zero");

    writeTempFile(source, "# deepen stale v2\n");
    expectTrue(cooker.cache().count_prunable_entries() == 1u, "stale entry is also prunable");
    expectTrue(cooker.cache().count_prune_all() == 1u, "count_prune_all matches stale count on valid cache");


    const fuse::u32 pruned = cooker.cache().prune_stale_entries();
    expectTrue(pruned == 1u, "prune removes probed stale entry");
    expectTrue(cooker.cache().count_stale_entries() == 0u, "count_stale_entries zero after prune");

void testCookCacheWouldInvalidateMirrors() {
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_source.obj", 42u),


    expectTrue(seeded.ok, "seed cook for would_invalidate mirrors ok");

               "would_invalidate_source mirrors count_by_source");
               "would_invalidate_output mirrors count_by_output");
               "matching hash would_invalidate_stale_content is false");
               "mismatched hash would_invalidate_stale_content is true");

void testCookCacheProbeStaleContentAndPruneEstimate() {
               "probe_stale_content on empty cache returns empty list");
    expectTrue(cache.estimate_prune_reconciliation().total() == 0u,
               "estimate_prune on empty cache returns zero total");

    const std::string source = writeTempFile("/tmp/fuse_b79_probe_stale_content.obj", "# probe stale v1\n");
    desc.output_path = "/tmp/fuse_b79_probe_stale_content.fusemesh";

    expectTrue(seeded.ok, "seed cook for stale content probe ok");
    expectTrue(cooker.cache().probe_stale_content_sources().empty(),
               "fresh entry is not reported as stale content");

    writeTempFile(source, "# probe stale v2\n");
    expectTrue(stale_sources.size() == 1u, "probe_stale_content reports one stale source");
    expectTrue(stale_sources[0] == source, "probe_stale_content returns changed source path");

    const fuse::project::CookCachePruneEstimate estimate = cooker.cache().estimate_prune_reconciliation();
    expectTrue(estimate.invalid_entries == 0u, "stale-only cache has zero invalid estimate");
    expectTrue(estimate.stale_entries == 1u, "stale-only cache estimates one stale entry");
    expectTrue(estimate.total() == cooker.cache().count_prunable_entries(),
               "prune estimate total matches count_prunable_entries");
    expectTrue(estimate.total() == cooker.cache().prune_all(),
               "prune estimate total matches prune_all removal count");

    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_upstream.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");



    expectTrue(!cooker.cache().would_invalidate_source(""),
               "would_invalidate_source rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_output(""),
               "would_invalidate_output rejects empty path");

    expectTrue(!cooker.cache().would_invalidate_stale_upstream_hashes({{source, 0u}}),
    expectTrue(cooker.cache().would_invalidate_stale_upstream_hashes({{source, 1u}}),
               "mismatched upstream hash is stale");

    expectTrue(cooker.cache().estimate_prune_removals() == cooker.cache().count_prunable_entries(),
               "estimate_prune_removals mirrors count_prunable_entries");

    expectTrue(cache.estimate_prune_all() == 0u, "estimate_prune_all on empty cache is zero");
    expectTrue(cache.count_stale_entries() == 0u, "count_stale on empty cache is zero");



               "would_invalidate_stale_content false when hash matches");
               "would_invalidate_stale_content true when hash mismatches");

    expectTrue(cooker.cache().count_stale_entries() == 1u, "count_stale reports one stale entry");
    expectTrue(cooker.cache().estimate_prune_all() == 1u, "estimate_prune_all matches stale count");

    const std::vector<std::pair<std::string, fuse::u64>> stale_probe = {
        {source, fuse::project::hash_mesh_import(desc)}};
    const std::vector<std::string> stale_sources =
        cooker.cache().probe_stale_content_sources(stale_probe);
    expectTrue(stale_sources.size() == 1u, "probe_stale_content_sources finds stale source");
    expectTrue(stale_sources[0] == source, "probe_stale_content_sources returns matching path");

    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_incr_probe.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_incr_probe.fusemesh"),
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_incr_probe.obj", 42u),
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_incr_probe.obj", 1u}}),
    expectTrue(cache.probe_prunable_source_paths().empty(),
               "probe_prunable_source_paths on empty cache returns empty");

    const std::string source = writeTempFile("/tmp/fuse_b79_incr_probe.obj", "# incr probe v1\n");
    desc.output_path = "/tmp/fuse_b79_incr_probe.fusemesh";



    writeTempFile(source, "# incr probe v2\n");
    expectTrue(updated_hash != seeded.content_hash, "content change alters mesh hash");

    const std::vector<std::string> prunable_sources = cooker.cache().probe_prunable_source_paths();
    expectTrue(prunable_sources.size() == 1u, "probe_prunable_source_paths finds stale source");
    expectTrue(prunable_sources[0] == source, "probe_prunable_source_paths returns stale source path");

void testCookHashPreflightFnvAndManifestCook() {
    const fuse::project::CookHashPreflight null_data = fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);
    expectTrue(!null_data.ok(), "null data with non-zero size fails FNV preflight");
    expectTrue(null_data.reason == fuse::project::CookHashRejectReason::NullData,
               "null data preflight reason is NullData");

    const fuse::project::CookHashPreflight valid_bytes = fuse::project::preflight_fnv1a64_bytes(&byte, 1u);
    expectTrue(valid_bytes.ok(), "non-null data passes FNV preflight");

    const fuse::project::CookHashPreflight empty_bytes = fuse::project::preflight_fnv1a64_bytes(nullptr, 0u);
    expectTrue(empty_bytes.ok(), "zero-size FNV preflight passes with null data");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_manifest.obj", "# manifest cook\n");
    const std::string dep = writeTempFile("/tmp/fuse_b79_preflight_dep.obj", "# manifest dep\n");

    fuse::project::CookManifestEntry dep_entry;
    dep_entry.kind = fuse::project::CookAssetKind::Mesh;
    dep_entry.source_path = dep;
    dep_entry.output_path = "/tmp/fuse_b79_preflight_dep.fusemesh";
    manifest.assets.push_back(dep_entry);

    entry.dependencies.push_back(dep_entry.output_path);
    manifest.assets.push_back(entry);

    const fuse::project::CookHashPreflight cook_preflight =
        fuse::project::preflight_manifest_cook_hash(entry, manifest);
    expectTrue(cook_preflight.ok(), "manifest cook hash preflight passes with valid dependency");
    expectTrue(fuse::project::hash_manifest_entry(entry) != 0,
               "manifest entry hash non-zero when cook preflight passes");

    fuse::project::CookManifestEntry no_dep = entry;
    no_dep.dependencies.clear();
    const fuse::project::CookHashPreflight no_dep_preflight =
        fuse::project::preflight_manifest_cook_hash(no_dep, manifest);
    expectTrue(no_dep_preflight.ok(), "manifest cook preflight passes without dependencies");

    no_dep.source_path = "";
    expectTrue(!fuse::project::preflight_manifest_cook_hash(no_dep, manifest).ok(),
               "manifest cook preflight rejects empty source path");

void testCookCacheSourceOutputAndPruneEstimators() {
    expectTrue(cache.estimate_prune_all() == 0u, "estimate_prune_all on empty cache returns zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_probe.obj", "# would probe\n");
    desc.output_path = "/tmp/fuse_b79_would_probe.fusemesh";

    expectTrue(seeded.ok, "seed cook for source/output estimators ok");

    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path would_invalidate guarded");
    expectTrue(!cooker.cache().would_invalidate_output(""), "empty output path would_invalidate guarded");
    expectTrue(cooker.cache().estimate_prune_all() == 0u, "fresh entry estimate_prune_all is zero");
    expectTrue(cooker.cache().count_stale_entries() == 0u, "fresh entry count_stale_entries is zero");

    writeTempFile(source, "# would probe updated\n");
    const fuse::u32 estimated = cooker.cache().estimate_prune_all();
    expectTrue(estimated == 1u, "estimate_prune_all matches stale count");
    expectTrue(cooker.cache().has_prunable_entries(), "stale entry marks cache prunable");

    expectTrue(removed == estimated, "prune_all removes estimated entries");
    expectTrue(cooker.cache().estimate_prune_all() == 0u, "estimate_prune_all zero after prune");





    const fuse::u64 current_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(current_hash != seeded.content_hash, "content change yields new hash key");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, current_hash),
               "would_invalidate_stale_content with stale hash is true");

void testCookCacheReconcileEstimate() {
    fuse::project::CookCache empty;
    const fuse::project::CookCacheReconcileEstimate empty_estimate = empty.estimate_reconcile();
    expectTrue(empty_estimate.invalid_entries == 0u, "empty cache reconcile invalid count is zero");
    expectTrue(empty_estimate.stale_content_entries == 0u, "empty cache reconcile stale content is zero");
    expectTrue(empty_estimate.stale_upstream_entries == 0u, "empty cache reconcile stale upstream is zero");
    expectTrue(empty_estimate.prunable_entries() == 0u, "empty cache prunable estimate is zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_reconcile_mesh.obj", "# reconcile v1\n");
    desc.output_path = "/tmp/fuse_b79_reconcile_mesh.fusemesh";


    const fuse::project::CookCacheReconcileEstimate fresh = cooker.cache().estimate_reconcile();
    expectTrue(fresh.invalid_entries == 0u, "fresh cache reconcile invalid count is zero");
    expectTrue(fresh.stale_content_entries == 0u, "fresh cache reconcile stale content is zero");
    expectTrue(fresh.stale_upstream_entries == 0u, "fresh cache reconcile stale upstream is zero");

    writeTempFile(source, "# reconcile v2\n");
    const fuse::project::CookCacheReconcileEstimate stale = cooker.cache().estimate_reconcile();
    expectTrue(stale.stale_content_entries == 1u, "stale content reconcile estimates one entry");
    expectTrue(stale.prunable_entries() == 1u, "stale content included in prunable estimate");
    expectTrue(stale.total_entries() == 1u, "stale reconcile total matches prunable count");

    expectTrue(removed == stale.prunable_entries(), "prune_all removes reconcile-estimated prunable count");

void testCookCacheStaleContentProbes() {

    const std::string source = writeTempFile("/tmp/fuse_b79_stale_probe.obj", "# stale probe v1\n");
    desc.output_path = "/tmp/fuse_b79_stale_probe.fusemesh";

    expectTrue(seeded.ok, "seed cook for stale content probes ok");
               "would_invalidate_source reports seeded source");
               "would_invalidate_output reports seeded output");
    expectTrue(cooker.cache().count_prune_all() == 0u, "count_prune_all on fresh cache returns zero");

    writeTempFile(source, "# stale probe v2\n");
    expectTrue(cooker.cache().count_prune_all() == 1u, "count_prune_all estimates stale removal");
    expectTrue(stale_sources.size() == 1u, "probe_stale_content finds one stale source");
    expectTrue(stale_sources[0] == source, "probe_stale_content returns matching source path");

    const fuse::u32 stale_before = cooker.cache().count_stale_entries();
    expectTrue(stale_before == 1u, "count_stale_entries reports stale entry before prune");
    const fuse::u32 removed = cooker.cache().prune_stale_entries();
    expectTrue(removed == stale_before, "prune stale removes probed stale entries");

    expectTrue(cache.count_stale_entries() == 0u, "count_stale on empty cache returns zero");


    expectTrue(seeded.ok, "seed cook for would-invalidate probes ok");


    expectTrue(cooker.cache().count_stale_entries() == 1u, "count_stale reports stale entry");
               "count_stale matches count_prunable for structurally valid stale entry");
    expectTrue(cooker.cache().probe_stale_content_sources().size() == 1u,
               "probe_stale_content lists stale source");
    expectTrue(cooker.cache().probe_stale_content_sources()[0] == source,
               "probe_stale_content returns matching source path");

    const fuse::project::CookCachePruneEstimate empty = cache.estimate_prune_removals();











    const fuse::project::CookHashPreflight null_bytes =
        fuse::project::preflight_fnv1a64_bytes(nullptr, 4u);





               "probe_stale_content empty after prune");

               "would_invalidate_downstream on empty cache is false");
    expectTrue(!cache.would_invalidate_all(), "would_invalidate_all on empty cache is false");

    const fuse::project::CookCacheInvalidationEstimate empty_estimate = cache.estimate_invalidate_all_removals();
    expectTrue(empty_estimate.all_entries == 0u, "empty cache invalidate-all estimate is zero");
    expectTrue(!empty_estimate.would_invalidate_all(), "empty estimate would_invalidate_all is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would invalidate\n");


               "matching hash does not would_invalidate stale content");
               "mismatched hash would_invalidate stale content");
    expectTrue(cooker.cache().would_invalidate_all(), "populated cache would_invalidate_all is true");
    expectTrue(cooker.cache().estimate_invalidate_all_removals().all_entries == 1u,
               "invalidate-all estimate matches entry count");


    invalid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    invalid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";

    const fuse::project::CookHashPreflight zero_hash = fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!zero_hash.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_hash.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,

    invalid.content_hash = 42;
    invalid.source_path = "";
    const fuse::project::CookHashPreflight empty_source = fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!empty_source.ok(), "empty source path fails cache entry preflight");
    expectTrue(empty_source.reason == fuse::project::CookHashRejectReason::EmptyInputPath,

    invalid.output_path = "";
    const fuse::project::CookHashPreflight empty_output = fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!empty_output.ok(), "empty output path fails cache entry preflight");
    expectTrue(empty_output.reason == fuse::project::CookHashRejectReason::EmptyOutputPath,

    expectTrue(fuse::project::preflight_cook_cache_entry(invalid).ok(), "valid cache entry passes preflight");
    expectTrue(fuse::project::is_valid_cook_cache_entry(invalid),
               "preflight success aligns with is_valid_cook_cache_entry");

void testCookHashPreflightCacheableAndManifestDeps() {
    expectTrue(!fuse::project::preflight_cacheable_cook_cache_key(0, 42u).ok(),
               "cacheable key preflight rejects zero source");
    expectTrue(fuse::project::preflight_cacheable_cook_cache_key(99u, 0).ok(),
               "cacheable key preflight allows zero upstream");
    expectTrue(fuse::project::preflight_cacheable_cook_cache_key(99u, 42u).ok(),
               "cacheable key preflight ok for valid fold");
    expectTrue(fuse::project::is_cacheable_cook_cache_key(99u, 42u),
               "cacheable preflight success implies cacheable fold");

    const std::string dep = writeTempFile("/tmp/fuse_b79_manifest_dep_preflight.obj", "# dep preflight\n");
    fuse::project::CookManifestEntry asset;
    asset.kind = fuse::project::CookAssetKind::Mesh;
    asset.source_path = dep;
    asset.output_path = "/tmp/fuse_b79_manifest_dep_preflight.fusemesh";
    manifest.assets.push_back(asset);

    const std::string source = writeTempFile("/tmp/fuse_b79_manifest_with_dep.obj", "# manifest with dep\n");
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_manifest_with_dep.fusemesh";
    entry.dependencies.push_back(asset.output_path);

    expectTrue(fuse::project::preflight_manifest_entry_with_dependencies_hash(entry, manifest).ok(),
               "manifest entry with readable dependency passes preflight");
               "manifest entry preflight ok without dependency walk");

    entry.dependencies = {""};
    expectTrue(!fuse::project::preflight_manifest_entry_with_dependencies_hash(entry, manifest).ok(),
               "all-empty dependency list fails manifest dependency preflight");


               "cacheable cache key preflight rejects zero source");
               "cacheable cache key preflight allows zero upstream");
               "cacheable cache key preflight ok for valid fold");

    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_inv.fusemesh", {}, {}),


    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""),
               "would_invalidate_source rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_output(""),
               "would_invalidate_output rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");

    writeTempFile(source, "# would invalidate revised\n");
    expectTrue(cooker.cache().would_prune_all(), "content change makes would_prune_all true");
    const fuse::u64 revised_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(revised_hash != seeded.content_hash, "revised content yields different hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, revised_hash),
               "would_invalidate_stale_content true when current hash differs from stored");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";

    fuse::project::CookCache cache;
    const fuse::project::CookHashPreflight zero_key = cache.preflight_cook_cache_entry(invalid);
    expectTrue(!zero_key.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_key.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash preflight reason");

    invalid.content_hash = 101;
    invalid.source_path = "";
    const fuse::project::CookHashPreflight empty_source = cache.preflight_cook_cache_entry(invalid);
    expectTrue(!empty_source.ok(), "empty source path fails cache entry preflight");
    expectTrue(empty_source.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path preflight reason");

    invalid.output_path = "";
    const fuse::project::CookHashPreflight empty_output = cache.preflight_cook_cache_entry(invalid);
    expectTrue(!empty_output.ok(), "empty output path fails cache entry preflight");
    expectTrue(empty_output.reason == fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output path preflight reason");

    const fuse::project::CookHashPreflight valid = cache.preflight_cook_cache_entry(invalid);
    expectTrue(valid.ok(), "structurally valid cache entry passes preflight");
    expectTrue(fuse::project::is_valid_cook_cache_entry(invalid),
               "preflight success aligns with is_valid_cook_cache_entry");

void testCookCacheWouldInvalidatePathProbes() {
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(!cache.would_invalidate_output(""), "would_invalidate_output rejects empty path");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would invalidate\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash is not stale for would_invalidate_stale_content");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash is stale for would_invalidate_stale_content");

    writeTempFile(source, "# would invalidate updated\n");
    expectTrue(cooker.cache().would_prune_all(), "stale content makes would_prune_all true");
    expectTrue(!cooker.cache().would_invalidate_stale_upstream_hashes({{source, 0u}}),
               "matching upstream hash does not trigger stale upstream would_invalidate");

void testCookCachePreflightCacheEntry() {

    const fuse::project::CookHashPreflight zero_hash = cache.preflight_cache_entry(invalid);
    expectTrue(!zero_hash.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_hash.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero hash cache entry preflight reason");

    invalid.content_hash = 42;
    const fuse::project::CookHashPreflight empty_source = cache.preflight_cache_entry(invalid);
    expectTrue(!empty_source.ok(), "empty source fails cache entry preflight");
               "empty source cache entry preflight reason");

    const fuse::project::CookHashPreflight empty_output = cache.preflight_cache_entry(invalid);
    expectTrue(!empty_output.ok(), "empty output fails cache entry preflight");
               "empty output cache entry preflight reason");

    const fuse::project::CookHashPreflight valid = cache.preflight_cache_entry(invalid);
    expectTrue(valid.ok(), "valid cache entry passes preflight");

void testCookCacheStaleUpstreamDedupProbe() {
    const std::string source = writeTempFile("/tmp/fuse_b79_upstream_dedup.obj", "# upstream dedup\n");

    desc.output_path = "/tmp/fuse_b79_upstream_dedup.fusemesh";

    expectTrue(seeded.ok, "seed cook for upstream dedup probe ok");

    const std::vector<std::pair<std::string, fuse::u64>> stale_upstream = {{source, seeded.content_hash + 1u}};
    const std::vector<std::string> raw =
        cooker.cache().probe_stale_upstream_sources(stale_upstream);
    const std::vector<std::string> deduped =
        cooker.cache().probe_stale_upstream_sources_dedup(stale_upstream);

    expectTrue(raw.size() == 1u, "single stale upstream entry in raw probe");
    expectTrue(deduped.size() == 1u, "deduped stale upstream probe has one source");
    expectTrue(deduped[0] == source, "deduped stale upstream probe returns source path");
    expectTrue(cooker.cache().would_invalidate_stale_upstream_hashes(stale_upstream),
               "would_invalidate_stale_upstream_hashes true for stale upstream");

void testCookCacheWouldInvalidateSourceAndOutputProbes() {
               "would_invalidate_source false on empty cache");
               "would_invalidate_output false on empty cache");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv.obj", "# would inv\n");
    desc.output_path = "/tmp/fuse_b79_would_inv.fusemesh";


    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source true for seeded entry");
               "would_invalidate_output true for seeded entry");
               "would_invalidate_stale_content false when hash matches");
               "would_invalidate_stale_content true when hash mismatches");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, 0u),
               "would_invalidate_stale_content rejects zero hash");

void testCookCachePreflightEntryAndManifestCoverage() {
    const fuse::project::CookHashPreflight zero_key = fuse::project::preflight_cook_cache_entry(invalid);

    invalid.content_hash = 42u;
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path fails cache entry preflight");

                   fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output path fails cache entry preflight");

    expectTrue(fuse::project::preflight_cook_cache_entry(invalid).ok(), "valid cache entry passes preflight");

    fuse::project::CookManifest manifest;
    expectTrue(!fuse::project::preflight_manifest_dependency_coverage({"/tmp/fuse_b79_missing_dep.fusemesh"},
                                                                      manifest)
                    .ok(),
               "missing manifest dependency fails coverage preflight");
    expectTrue(fuse::project::preflight_manifest_dependency_coverage({"/tmp/fuse_b79_missing_dep.fusemesh"},
                       .reason == fuse::project::CookHashRejectReason::MissingManifestDependency,
               "missing manifest dependency preflight reason");
    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::MissingManifestDependency)) == "missing_manifest_dependency",
               "missing manifest dependency label");

void testCookCacheStaleUpstreamUniqueProbe() {
    expectTrue(cache.probe_stale_upstream_sources_unique({{"/tmp/fuse_b79_up_unique.obj", 1u}}).empty(),
               "unique upstream probe empty on empty cache");

    fuse::project::CookCacheEntry entry_a;
    entry_a.content_hash = 901;
    entry_a.source_path = "/tmp/fuse_b79_up_unique_a.obj";
    entry_a.output_path = "/tmp/fuse_b79_up_unique_a.fusemesh";
    entry_a.upstream_hash = 1;
    cache.store(entry_a);

    fuse::project::CookCacheEntry entry_b;
    entry_b.content_hash = 902;
    entry_b.source_path = "/tmp/fuse_b79_up_unique_a.obj";
    entry_b.output_path = "/tmp/fuse_b79_up_unique_b.fusemesh";
    entry_b.upstream_hash = 1;
    cache.store(entry_b);

    const std::vector<std::pair<std::string, fuse::u64>> stale_upstream = {
        {entry_a.source_path, 99u},
    };
    expectTrue(cache.probe_stale_upstream_sources(stale_upstream).size() == 2u,
               "non-unique upstream probe lists each matching entry");
    expectTrue(cache.would_invalidate_stale_upstream_hashes(stale_upstream),
               "would_invalidate_stale_upstream true when hashes mismatch");

    const std::vector<std::string> unique = cache.probe_stale_upstream_sources_unique(stale_upstream);
    expectTrue(unique.size() == 1u, "unique upstream probe deduplicates source paths");
    expectTrue(unique[0] == entry_a.source_path, "unique upstream probe returns stale source");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({}), "empty upstream list would_invalidate is false");

void testCookCacheWouldInvalidateProbes() {
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");




    expectTrue(cooker.cache().would_invalidate(seeded.content_hash),
               "would_invalidate still true for stale hash key");

    fuse::project::CookCacheEntry invalid_key;
    invalid_key.content_hash = 0;
    invalid_key.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_key.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(!fuse::project::preflight_cook_cache_entry(invalid_key).ok(),
               "zero content hash fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid_key).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,

    fuse::project::CookCacheEntry invalid_source = invalid_key;
    invalid_source.content_hash = 42;
    invalid_source.source_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid_source).reason ==

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42;
    valid.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");
    expectTrue(fuse::project::is_valid_cook_cache_entry(valid), "preflight success matches validity helper");

void testCookHashManifestDependencyPreflight() {
    const std::string source = writeTempFile("/tmp/fuse_b79_manifest_dep_src.obj", "# manifest dep src\n");
    const std::string dependency = writeTempFile("/tmp/fuse_b79_manifest_dep_file.obj", "# manifest dep file\n");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_manifest_dep_src.fusemesh";
    entry.dependencies = {"", dependency};

    expectTrue(fuse::project::preflight_manifest_entry_dependencies(entry).ok(),
               "readable manifest dependencies pass preflight");

    entry.dependencies = {"/tmp/fuse_b79_missing_manifest_dep.obj"};
    expectTrue(!fuse::project::preflight_manifest_entry_dependencies(entry).ok(),
               "missing manifest dependency fails preflight");
    expectTrue(fuse::project::preflight_manifest_entry_dependencies(entry).reason ==
                   fuse::project::CookHashRejectReason::SourceUnreadable,

void testCookHashPreflightCacheableKeyGuards() {
    expectTrue(!fuse::project::preflight_cacheable_cook_cache_key(0, 0).ok(),
               "zero fold fails cacheable key preflight");
    expectTrue(fuse::project::preflight_cacheable_cook_cache_key(0, 0).reason ==
               "zero fold cacheable preflight reason");

    expectTrue(fuse::project::preflight_cacheable_cook_cache_key(99u, 0).ok(),
               "valid source-only fold passes cacheable preflight");
    expectTrue(fuse::project::preflight_cacheable_cook_cache_key(99u, 42u).ok(),
               "valid combined fold passes cacheable preflight");
    expectTrue(fuse::project::is_cacheable_cook_cache_key(99u, 42u),
               "cacheable preflight agrees with inline helper");

    valid.content_hash = 501;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";

    const fuse::project::CookCacheEntryPreflight ok =
        fuse::project::preflight_cook_cache_entry(valid);
    expectTrue(ok.ok(), "valid cache entry passes store preflight");
    expectTrue(fuse::project::is_valid_cook_cache_entry(valid),
               "store preflight agrees with entry validation");

    valid.content_hash = 0;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).reason ==
               "zero hash entry preflight reason");

    expectTrue(!cache.would_invalidate_all(), "would_invalidate_all on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_down.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");



    expectTrue(cooker.cache().would_invalidate_all(), "would_invalidate_all on populated cache is true");

void testCookCacheUniqueStaleUpstreamProbe() {
    const std::string source = writeTempFile("/tmp/fuse_b79_unique_upstream.obj", "# unique upstream\n");
    desc.output_path = "/tmp/fuse_b79_unique_upstream.fusemesh";

    expectTrue(seeded.ok, "seed cook for unique upstream probe ok");

    const std::vector<std::pair<std::string, fuse::u64>> stale_pairs = {
        {source, seeded.content_hash + 1u},
        {source, seeded.content_hash + 2u},

        cooker.cache().probe_stale_upstream_sources(stale_pairs);
    const std::vector<std::string> unique =
        cooker.cache().probe_unique_stale_upstream_sources(stale_pairs);

    expectTrue(raw.size() == 2u, "raw stale upstream probe reports one push per matching pair");
    expectTrue(unique.size() == 1u, "unique stale upstream probe deduplicates source path");
    expectTrue(unique[0] == source, "unique stale upstream probe preserves source path");

void testCookHashPreflightMtimeAndDependencyGuards() {
    const fuse::project::CookHashPreflight empty_mtime = fuse::project::preflight_file_mtime_ns("");
    expectTrue(!empty_mtime.ok(), "empty path fails mtime preflight");
    expectTrue(empty_mtime.reason == fuse::project::CookHashRejectReason::EmptyPath,
               "empty path mtime preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_mtime.obj", "# mtime preflight\n");
    expectTrue(fuse::project::preflight_file_mtime_ns(source).ok(), "readable path passes mtime preflight");

    const fuse::project::CookHashPreflight zero_combine = fuse::project::preflight_fnv1a64_combine(0, 42u);
    expectTrue(!zero_combine.ok(), "zero left operand fails combine preflight");
    expectTrue(zero_combine.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero left combine preflight reason");
    expectTrue(fuse::project::preflight_fnv1a64_combine(99u, 0).ok(), "non-zero left passes combine preflight");

    expectTrue(!fuse::project::preflight_upstream_dependency_path("", manifest).ok(),
               "empty dependency path fails preflight");
    expectTrue(fuse::project::preflight_upstream_dependency_path("/tmp/fuse_b79_missing_dep.fusemesh", manifest)
                       .reason == fuse::project::CookHashRejectReason::UnresolvedDependency,
               "unknown dependency path fails preflight");

    entry.output_path = "/tmp/fuse_b79_preflight_dep.fusemesh";
    manifest.assets.push_back(entry);

    expectTrue(fuse::project::preflight_upstream_dependency_path(entry.output_path, manifest).ok(),
               "resolved dependency path passes preflight");
    expectTrue(fuse::project::preflight_upstream_dependencies_hash({entry.output_path}, manifest).ok(),
               "upstream list preflight resolves manifest dependency");

                   fuse::project::CookHashRejectReason::UnresolvedDependency)) == "unresolved_dependency",
               "reject reason label for unresolved dependency");

void testCookCacheInvalidationEstimateGuards() {
    const fuse::project::CookCacheInvalidationEstimate empty =
        cache.estimate_invalidation_removals("/tmp/fuse_b79_est_source.obj", "/tmp/fuse_b79_est_output.fusemesh",
                                             42u, {{" /tmp/fuse_b79_est_source.obj", 1u}});
    expectTrue(empty.total() == 0u, "empty cache invalidation estimate is zero");
    expectTrue(!cache.would_invalidate_any("/tmp/fuse_b79_est_source.obj"), "empty cache would_invalidate_any false");
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_est_source.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_est_output.fusemesh"),
    expectTrue(cache.count_stale_upstream_sources({{"/tmp/fuse_b79_est_source.obj", 1u}}) == 0u,
               "deduplicated stale upstream count zero on empty cache");
    expectTrue(cache.probe_stale_upstream_sources_deduplicated({{"/tmp/fuse_b79_est_source.obj", 1u}}).empty(),
               "deduplicated stale upstream probe empty on empty cache");

    const std::string source = writeTempFile("/tmp/fuse_b79_est_inv.obj", "# est inv\n");
    desc.output_path = "/tmp/fuse_b79_est_inv.fusemesh";

    expectTrue(seeded.ok, "seed cook for invalidation estimate ok");

    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path guarded");
    expectTrue(!cooker.cache().would_invalidate_output(""), "empty output path guarded");

    const fuse::project::CookCacheInvalidationEstimate estimate =
        cooker.cache().estimate_invalidation_removals(source, desc.output_path, seeded.content_hash);
    expectTrue(estimate.by_source_path == 1u, "estimate counts source-path entries");
    expectTrue(estimate.by_output_path == 1u, "estimate counts output-path entries");
    expectTrue(estimate.stale_content == 0u, "matching content hash yields zero stale-content estimate");
    expectTrue(cooker.cache().would_invalidate_any(source, desc.output_path, seeded.content_hash),
               "would_invalidate_any true for populated cache");

    writeTempFile(source, "# est inv revised\n");
    const fuse::u64 revised_hash = seeded.content_hash + 1u;
    const fuse::project::CookCacheInvalidationEstimate stale =
        cooker.cache().estimate_invalidation_removals(source, "", revised_hash);
    expectTrue(stale.stale_content == 1u, "stale content estimate reports mismatched hash");


    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would mesh\n");


    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output rejects empty path");
               "would_invalidate_stale_content with matching hash is false");
               "would_invalidate_stale_content with mismatched hash is true");

    writeTempFile(source, "# would mesh updated\n");
    expectTrue(cooker.cache().would_prune_all(), "stale entry makes would_prune_all true before prune");
               "would_invalidate still true for stale but present entry");

    const fuse::u32 removed = cooker.cache().prune_stale_entries();
    expectTrue(removed == 1u, "prune removes entry probed by would_invalidate");
    expectTrue(!cooker.cache().would_invalidate(seeded.content_hash),
               "would_invalidate false after prune removes entry");

    const fuse::project::CookHashPreflight zero_key = fuse::project::preflight_cook_cache_entry({});
    expectTrue(!zero_key.ok(), "default cache entry fails preflight");

    fuse::project::CookCacheEntry missing_source;
    missing_source.content_hash = 501;
    missing_source.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(!fuse::project::preflight_cook_cache_entry(missing_source).ok(),
               "missing source path fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(missing_source).reason ==
               "missing source path preflight reason");

    fuse::project::CookCacheEntry missing_output;
    missing_output.content_hash = 502;
    missing_output.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    expectTrue(!fuse::project::preflight_cook_cache_entry(missing_output).ok(),
               "missing output path fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(missing_output).reason ==
               "missing output path preflight reason");

    valid.content_hash = 503;
    valid.source_path = "/tmp/fuse_b79_preflight_entry_valid.obj";
    valid.output_path = "/tmp/fuse_b79_preflight_entry_valid.fusemesh";
               "preflight success implies structural validity");

void testCookHashPreflightManifestDependencyOutputs() {
    const std::string dep = writeTempFile("/tmp/fuse_b79_preflight_dep.obj", "# dep\n");

    fuse::project::CookManifestEntry asset;
    asset.kind = fuse::project::CookAssetKind::Mesh;
    asset.source_path = dep;
    asset.output_path = "/tmp/fuse_b79_preflight_dep.fusemesh";
    manifest.assets.push_back(asset);

    fuse::project::CookManifestEntry consumer;
    consumer.kind = fuse::project::CookAssetKind::Mesh;
    consumer.source_path = writeTempFile("/tmp/fuse_b79_preflight_consumer.obj", "# consumer\n");
    consumer.output_path = "/tmp/fuse_b79_preflight_consumer.fusemesh";
    consumer.dependencies.push_back(asset.output_path);

    const fuse::project::CookHashPreflight resolved =
        fuse::project::preflight_manifest_dependency_outputs(consumer, manifest);
    expectTrue(resolved.ok(), "resolved manifest dependency passes preflight");

    consumer.dependencies = {"/tmp/fuse_b79_missing_output.fusemesh"};
    const fuse::project::CookHashPreflight unresolved =
    expectTrue(!unresolved.ok(), "missing dependency output fails preflight");
    expectTrue(unresolved.reason == fuse::project::CookHashRejectReason::UnresolvedDependencyOutput,
               "unresolved dependency preflight reason");

    consumer.dependencies.clear();
    expectTrue(!fuse::project::preflight_manifest_dependency_outputs(consumer, manifest).ok(),
               "empty dependency list fails manifest dependency preflight");

void testCookHashPreflightFileMtime() {
    expectTrue(!fuse::project::preflight_file_mtime("").ok(), "empty path fails mtime preflight");
    expectTrue(fuse::project::preflight_file_mtime("").reason == fuse::project::CookHashRejectReason::EmptyPath,

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_mtime.obj", "# mtime\n");
    expectTrue(fuse::project::preflight_file_mtime(source).ok(), "non-empty path passes mtime preflight");
    expectTrue(fuse::project::file_mtime_ns(source) != 0, "mtime preflight success implies readable mtime");

    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_inv.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_inv.fusemesh"),
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_inv.obj", 42u),
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_inv.obj", 1u}}),
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_inv.fusemesh", {}, {}),

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv_mesh.obj", "# would inv\n");
    desc.output_path = "/tmp/fuse_b79_would_inv_mesh.fusemesh";


               "matching hash does not would_invalidate stale content");
               "mismatched hash would_invalidate stale content");

    writeTempFile(source, "# would inv updated\n");
    expectTrue(cooker.cache().count_prunable_entries() == 1u,
               "disk content change marks entry prunable for stale reconcile");
    expectTrue(!cooker.cache().probe_stale_content_sources().empty(),
               "stale content probe reports changed source after disk rewrite");

    const fuse::project::CookCacheInvalidationEstimate empty_source =
        cache.estimate_invalidation_for_source("/tmp/fuse_b79_est_inv.obj");
    expectTrue(empty_source.total() == 0u, "empty cache source estimate is zero");

    valid.content_hash = 909;
    valid.source_path = "/tmp/fuse_b79_est_inv.obj";
    valid.output_path = "/tmp/fuse_b79_est_inv.fusemesh";
    cache.store(valid);
    expectTrue(cache.entry_count() == 1u, "valid entry stored for invalidation estimate");

    const fuse::project::CookCacheInvalidationEstimate source_estimate =
        cache.estimate_invalidation_for_source(valid.source_path);
    expectTrue(source_estimate.source_entries == 1u, "source estimate counts seeded entry");
    expectTrue(cache.would_invalidate_source(valid.source_path), "would_invalidate matches source estimate");

    const fuse::project::CookCacheInvalidationEstimate output_estimate =
        cache.estimate_invalidation_for_output(valid.output_path);
    expectTrue(output_estimate.output_entries == 1u, "output estimate counts seeded entry");
    expectTrue(cache.would_invalidate_output(valid.output_path), "would_invalidate matches output estimate");

    const fuse::project::CookCacheInvalidationEstimate stale_estimate =
        cache.estimate_stale_content_invalidation(valid.source_path, valid.content_hash + 1u);
    expectTrue(stale_estimate.stale_content_entries == 1u, "stale content estimate counts mismatched hash");

void testCookCachePreflightStoreEntry() {

    invalid.source_path = "/tmp/fuse_b79_preflight_store.obj";
    invalid.output_path = "/tmp/fuse_b79_preflight_store.fusemesh";
    const fuse::project::CookHashPreflight zero_key = cache.preflight_store_entry(invalid);
    expectTrue(!zero_key.ok(), "zero hash fails store preflight");
               "zero hash store preflight reason");

    invalid.content_hash = 707;
    expectTrue(cache.preflight_store_entry(invalid).reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source fails store preflight");

    expectTrue(cache.preflight_store_entry(invalid).reason == fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output fails store preflight");

    expectTrue(cache.preflight_store_entry(invalid).ok(), "valid entry passes store preflight");
    cache.store(invalid);
    expectTrue(cache.entry_count() == 1u, "preflight-valid entry stores");

void testCookCacheWouldInvalidateSourceOutputProbes() {

    const std::string source = writeTempFile("/tmp/fuse_b79_would_probe.obj", "# would probe\n");
    desc.output_path = "/tmp/fuse_b79_would_probe.fusemesh";


    expectTrue(!cache.would_invalidate_source(""), "empty source path would_invalidate is guarded");
    expectTrue(!cooker.cache().would_invalidate_output(""),
               "empty output path would_invalidate is guarded");
               "matching hash would_invalidate_stale_content is false");
               "mismatched hash would_invalidate_stale_content is true");

void testCookCacheProbeStaleUpstreamSourcesUnique() {
    const std::string source = writeTempFile("/tmp/fuse_b79_upstream_unique.obj", "# upstream unique\n");

    entry.output_path = "/tmp/fuse_b79_upstream_unique.fusemesh";

    expectTrue(cooker.cook_entry(entry, manifest).ok, "seed cook for unique upstream probe");

    const fuse::u64 stale_upstream = 999u;
    const std::vector<std::pair<std::string, fuse::u64>> pairs = {{source, stale_upstream},
                                                                  {source, stale_upstream}};

    const std::vector<std::string> duplicated = cooker.cache().probe_stale_upstream_sources(pairs);
    expectTrue(duplicated.size() == 2u, "non-unique upstream probe may duplicate per entry");

    const std::vector<std::string> unique = cooker.cache().probe_stale_upstream_sources_unique(pairs);
    expectTrue(unique[0] == source, "unique upstream probe returns stale source");
    expectTrue(cooker.cache().probe_stale_upstream_sources_unique({}).empty(),
               "empty upstream pair list yields empty unique probe");

void testCookHashShaderAndManifestUpstreamPreflights() {
    fuse::project::CookManifestEntry shader;
    shader.kind = fuse::project::CookAssetKind::Shader;
    shader.source_path = "/tmp/fuse_b79_shader_preflight.glsl";
    shader.output_path = "/tmp/fuse_b79_shader_preflight.fuseshader";
    expectTrue(fuse::project::preflight_shader_entry_hash(shader).ok(),
               "shader entry with paths passes path-only preflight");

    shader.source_path = "";
    expectTrue(fuse::project::preflight_shader_entry_hash(shader).reason ==
               "empty shader input path preflight reason");

    const std::string dep = writeTempFile("/tmp/fuse_b79_manifest_upstream_dep.obj", "# upstream dep\n");
    const std::string consumer = writeTempFile("/tmp/fuse_b79_manifest_upstream_consumer.obj", "# consumer\n");

    fuse::project::CookManifestEntry producer;
    producer.kind = fuse::project::CookAssetKind::Mesh;
    producer.source_path = dep;
    producer.output_path = "/tmp/fuse_b79_manifest_upstream_dep.fusemesh";
    manifest.assets.push_back(producer);

    fuse::project::CookManifestEntry dependent;
    dependent.kind = fuse::project::CookAssetKind::Mesh;
    dependent.source_path = consumer;
    dependent.output_path = "/tmp/fuse_b79_manifest_upstream_consumer.fusemesh";
    dependent.dependencies.push_back(producer.output_path);
    manifest.assets.push_back(dependent);

    const fuse::project::CookHashPreflight with_upstream =
        fuse::project::preflight_manifest_entry_with_upstream(dependent, manifest);
    expectTrue(with_upstream.ok(), "manifest entry with upstream deps passes combined preflight");

    fuse::project::CookManifestEntry no_deps = dependent;
    no_deps.dependencies.clear();
    expectTrue(fuse::project::preflight_manifest_entry_with_upstream(no_deps, manifest).ok(),
               "manifest entry without deps skips upstream preflight");

void testCookCacheWouldInvalidationProbes() {




    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content with stale hash is true");
    expectTrue(cooker.cache().count_stale_content_for_source(source, seeded.content_hash) == 1u,
               "count_stale_content matches would_invalidate_stale_content");


    fuse::project::CookCacheEntry zero_hash = valid;
    zero_hash.content_hash = 0;
    const fuse::project::CookHashPreflight zero_preflight =
        fuse::project::preflight_cook_cache_entry(zero_hash);
    expectTrue(!zero_preflight.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_preflight.reason == fuse::project::CookHashRejectReason::ZeroContentHash,

    fuse::project::CookCacheEntry empty_source = valid;
    empty_source.source_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==

    fuse::project::CookCacheEntry empty_output = valid;
    empty_output.output_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_output).reason ==

                   fuse::project::CookHashRejectReason::ZeroContentHash)) == "zero_content_hash",
               "reject reason label for zero content hash");























    expectTrue(cooker.cache().would_invalidate_all(), "would_invalidate_all true on populated cache");

    const fuse::u64 updated_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(updated_hash != seeded.content_hash, "source change yields new content hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, updated_hash),

    expectTrue(cooker.cache().invalidate_source(source) == 1u, "source invalidation removes probed entry");
    expectTrue(!cooker.cache().would_invalidate_all(), "would_invalidate_all false after source invalidation");

void testCookCacheUniqueStaleUpstreamCount() {

    entry_a.source_path = "/tmp/fuse_b79_unique_upstream_a.obj";
    entry_a.output_path = "/tmp/fuse_b79_unique_upstream_a.fusemesh";

    fuse::project::CookCacheEntry entry_b = entry_a;
    entry_b.source_path = "/tmp/fuse_b79_unique_upstream_b.obj";
    entry_b.output_path = "/tmp/fuse_b79_unique_upstream_b.fusemesh";

        {entry_a.source_path, 2u},
        {entry_b.source_path, 2u},

    expectTrue(cache.count_stale_upstream_hashes(stale_pairs) == 3u,
               "stale upstream count includes duplicate source matches");
    expectTrue(cache.count_unique_stale_upstream_sources(stale_pairs) == 2u,
               "unique stale upstream count deduplicates sources");
    expectTrue(cache.probe_stale_upstream_sources(stale_pairs).size() == 3u,
               "stale upstream probe preserves per-entry matches");
}

void testCookHashPreflightImportPathsAndCacheEntry() {
    const fuse::project::CookHashPreflight empty_input =
        fuse::project::preflight_import_paths("", "/tmp/fuse_b79_import_paths_out.fusemesh");
    expectTrue(!empty_input.ok(), "empty import input path fails preflight");
    expectTrue(empty_input.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty import input path reason");

    const fuse::project::CookHashPreflight empty_output =
        fuse::project::preflight_import_paths("/tmp/fuse_b79_import_paths_in.obj", "");
    expectTrue(!empty_output.ok(), "empty import output path fails preflight");
    expectTrue(empty_output.reason == fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty import output path reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry.obj", "# cache entry\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 909;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry preflight ok");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    expectTrue(fuse::project::preflight_cook_cache_entry(zero_key).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero cache key fails entry preflight");

    fuse::project::CookCacheEntry shader_entry;
    shader_entry.content_hash = 910;
    shader_entry.source_path = "/tmp/fuse_b79_shader_preflight.obj";
    shader_entry.output_path = "/tmp/fuse_b79_shader_preflight.fuseshader";
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    expectTrue(fuse::project::preflight_cook_cache_entry(shader_entry).ok(),
               "shader cache entry passes structural preflight without source file");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv.obj", "# would inv\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_inv.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded source");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded output");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash is not stale for would_invalidate_stale_content");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash is stale for would_invalidate_stale_content");
    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path would_invalidate is guarded");
}

void testCookCacheProbeUniqueStaleUpstreamSources() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_unique_upstream_a.obj", "# unique a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_unique_upstream_b.obj", "# unique b\n");

    fuse::project::MeshImportDesc desc_a;
    desc_a.input_path = source_a;
    desc_a.output_path = "/tmp/fuse_b79_unique_upstream_a.fusemesh";

    fuse::project::MeshImportDesc desc_b;
    desc_b.input_path = source_b;
    desc_b.output_path = "/tmp/fuse_b79_unique_upstream_b.fusemesh";

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_mesh(desc_a).ok, "seed cook a for unique upstream probe");
    expectTrue(cooker.cook_mesh(desc_b).ok, "seed cook b for unique upstream probe");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries for unique upstream probe");

    const std::vector<std::pair<std::string, fuse::u64>> stale_pairs = {
        {source_a, 1u},
        {source_a, 1u},
        {source_b, 2u},
    };
    const std::vector<std::string> per_entry =
        cooker.cache().probe_stale_upstream_sources(stale_pairs);
    expectTrue(per_entry.size() == 3u, "per-entry stale upstream probe counts each match");

    const std::vector<std::string> unique =
        cooker.cache().probe_unique_stale_upstream_sources(stale_pairs);
    expectTrue(unique.size() == 2u, "unique stale upstream probe deduplicates sources");
    expectTrue(unique[0] == source_a || unique[1] == source_a, "source a in unique upstream probe");
    expectTrue(unique[0] == source_b || unique[1] == source_b, "source b in unique upstream probe");
}

void testCookCacheWouldInvalidateShortcuts() {
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

    const std::string source = writeTempFile("/tmp/fuse_b79_would_shortcut.obj", "# would shortcut\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_shortcut.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate shortcuts ok");

    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false for matching hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true for mismatched hash");

    writeTempFile(source, "# would shortcut revised\n");
    expectTrue(cooker.cache().would_prune_all(), "stale content makes would_prune_all true");
    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source still true while stale entry remains cached");
    expectTrue(cooker.cache().prune_stale_entries() == 1u, "stale prune removes revised entry");
    expectTrue(!cooker.cache().would_invalidate_source(source),
               "would_invalidate_source false after stale entry pruned");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_source;
    invalid_source.content_hash = 901u;
    invalid_source.source_path = "";
    invalid_source.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight empty_source = fuse::project::preflight_cook_cache_entry(invalid_source);
    expectTrue(!empty_source.ok(), "empty source path fails cache entry preflight");
    expectTrue(empty_source.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path preflight reason");

    fuse::project::CookCacheEntry invalid_output = invalid_source;
    invalid_output.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_output.output_path = "";
    const fuse::project::CookHashPreflight empty_output = fuse::project::preflight_cook_cache_entry(invalid_output);
    expectTrue(!empty_output.ok(), "empty output path fails cache entry preflight");
    expectTrue(empty_output.reason == fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output path preflight reason");

    fuse::project::CookCacheEntry zero_hash = invalid_output;
    zero_hash.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    zero_hash.content_hash = 0;
    expectTrue(!fuse::project::preflight_cook_cache_entry(zero_hash).ok(),
               "zero content hash fails cache entry preflight");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry_ok.obj", "# entry ok\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_preflight_entry_ok.fusemesh";
    const fuse::u64 content_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(content_hash != 0, "valid source produces non-zero hash for entry preflight");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = content_hash;
    valid.source_path = source;
    valid.output_path = desc.output_path;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");
}

void testCookCacheUniqueStaleUpstreamProbes() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry entry_a;
    entry_a.content_hash = 1001u;
    entry_a.upstream_hash = 10u;
    entry_a.source_path = "/tmp/fuse_b79_unique_up_a.obj";
    entry_a.output_path = "/tmp/fuse_b79_unique_up_a.fusemesh";
    cache.store(entry_a);

    fuse::project::CookCacheEntry entry_b;
    entry_b.content_hash = 1002u;
    entry_b.upstream_hash = 10u;
    entry_b.source_path = "/tmp/fuse_b79_unique_up_a.obj";
    entry_b.output_path = "/tmp/fuse_b79_unique_up_b.fusemesh";
    cache.store(entry_b);

    fuse::project::CookCacheEntry entry_c;
    entry_c.content_hash = 1003u;
    entry_c.upstream_hash = 20u;
    entry_c.source_path = "/tmp/fuse_b79_unique_up_c.obj";
    entry_c.output_path = "/tmp/fuse_b79_unique_up_c.fusemesh";
    cache.store(entry_c);

    const std::vector<std::pair<std::string, fuse::u64>> upstream_pairs = {
        {"/tmp/fuse_b79_unique_up_a.obj", 99u},
        {"/tmp/fuse_b79_unique_up_c.obj", 99u},
    };

    const std::vector<std::string> raw = cache.probe_stale_upstream_sources(upstream_pairs);
    expectTrue(raw.size() == 3u, "raw stale upstream probe counts each matching entry");

    const std::vector<std::string> unique = cache.probe_unique_stale_upstream_sources(upstream_pairs);
    expectTrue(unique.size() == 2u, "unique stale upstream probe deduplicates source paths");
    expectTrue(cache.count_unique_stale_upstream_sources(upstream_pairs) == 2u,
               "unique stale upstream count matches deduplicated probe");
    expectTrue(cache.would_invalidate_stale_upstream_hashes(upstream_pairs),
               "would_invalidate_stale_upstream true when stale upstream entries exist");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_unique_up_c.obj", 20u}}),
               "would_invalidate_stale_upstream false when upstream hashes match");
}

void testCookCacheWouldInvalidateProbes() {
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

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would invalidate\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash does not would_invalidate stale content");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash would_invalidate stale content");

    writeTempFile(source, "# would invalidate revised\n");
    fuse::project::MeshImportDesc revised = desc;
    const fuse::u64 revised_hash = fuse::project::hash_mesh_import(revised);
    expectTrue(revised_hash != seeded.content_hash, "content change yields new hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, revised_hash),
               "revised hash would_invalidate stale stored entry");

    expectTrue(!cache.would_invalidate_source(""), "empty source path would_invalidate is guarded");
    expectTrue(!cache.would_invalidate_output(""), "empty output path would_invalidate is guarded");
}

void testCookCacheProbeInvalidEntrySources() {
    fuse::project::CookCache cache;
    expectTrue(cache.probe_invalid_entry_sources().empty(), "empty cache invalid source probe is empty");

    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_probe_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_probe_invalid.fusemesh";
    cache.store(invalid);
    expectTrue(cache.entry_count() == 0u, "invalid entry not stored");

    const std::string cachePath = "/tmp/fuse_b79_probe_invalid_only.json";
    {
        std::ofstream out(cachePath, std::ios::binary);
        out << R"({
  "schemaVersion": 1,
  "entries": [
    {
      "contentHash": 0,
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_probe_invalid_only.fusemesh",
      "sourcePath": "/tmp/fuse_b79_probe_invalid_only.obj",
      "kind": "mesh"
    }
  ]
}
)";
    }

    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "invalid-only cache JSON loads");
    expectTrue(loaded.empty(), "invalid-only cache rejects zero-hash entry on load");
    expectTrue(loaded.probe_invalid_entry_sources().empty(),
               "rejected invalid entry does not appear in invalid source probe");
}

void testCookHashPreflightCacheEntryAndManifestDeps() {
    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry.obj", "# preflight entry\n");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");

    fuse::project::CookCacheEntry shader_entry = valid;
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    const fuse::project::CookHashPreflight shader_preflight =
        fuse::project::preflight_cook_cache_entry(shader_entry);
    expectTrue(!shader_preflight.ok(), "shader cache entry fails preflight");
    expectTrue(shader_preflight.reason == fuse::project::CookHashRejectReason::UnsupportedAssetKind,
               "shader cache entry preflight reason");

    valid.content_hash = 0;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero hash cache entry preflight reason");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::preflight_manifest_entry_with_dependencies(entry, manifest).ok(),
               "manifest entry without deps passes preflight");

    const std::string dep = writeTempFile("/tmp/fuse_b79_preflight_dep.obj", "# preflight dep\n");
    fuse::project::CookManifestEntry dep_asset;
    dep_asset.kind = fuse::project::CookAssetKind::Mesh;
    dep_asset.source_path = dep;
    dep_asset.output_path = "/tmp/fuse_b79_preflight_dep.fusemesh";
    manifest.assets.push_back(dep_asset);
    entry.dependencies.push_back(dep_asset.output_path);
    expectTrue(fuse::project::preflight_manifest_entry_with_dependencies(entry, manifest).ok(),
               "manifest entry with readable dependency passes preflight");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::UnsupportedAssetKind)) == "unsupported_asset_kind",
               "reject reason label for unsupported asset kind");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_hash;
    invalid_hash.content_hash = 0;
    invalid_hash.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    invalid_hash.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    const fuse::project::CookHashPreflight zero_hash =
        fuse::project::preflight_cook_cache_entry(invalid_hash);
    expectTrue(!zero_hash.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_hash.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash preflight reason");

    fuse::project::CookCacheEntry invalid_source = invalid_hash;
    invalid_source.content_hash = 909;
    invalid_source.source_path = "";
    const fuse::project::CookHashPreflight empty_source =
        fuse::project::preflight_cook_cache_entry(invalid_source);
    expectTrue(!empty_source.ok(), "empty source path fails cache entry preflight");
    expectTrue(empty_source.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_entry_preflight_ok.obj", "# entry preflight\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 910;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_entry_preflight_ok.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(),
               "readable mesh cache entry passes preflight");

    fuse::project::CookCacheEntry shader_entry = valid;
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    expectTrue(fuse::project::preflight_cook_cache_entry(shader_entry).ok(),
               "shader cache entry passes structural preflight without source read");
}

void testCookCacheWouldInvalidateGuards() {
    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv.obj", "# would inv\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_inv.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate guards ok");

    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source would_invalidate is guarded");
    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash would_invalidate_stale_content is false");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash would_invalidate_stale_content is true");
    expectTrue(!cooker.cache().would_invalidate(0), "zero hash would_invalidate is guarded");
}

void testCookCacheUniqueStaleUpstreamProbes() {
    const std::string source = writeTempFile("/tmp/fuse_b79_unique_upstream.obj", "# unique upstream\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_unique_upstream.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for unique upstream probe ok");

    const std::vector<std::pair<std::string, fuse::u64>> stale_pairs = {
        {source, seeded.content_hash + 1u},
        {source, seeded.content_hash + 2u},
    };

    expectTrue(cooker.cache().count_stale_upstream_hashes(stale_pairs) == 2u,
               "entry-level stale upstream count includes one increment per matching pair");
    expectTrue(cooker.cache().count_unique_stale_upstream_hashes(stale_pairs) == 1u,
               "unique stale upstream count deduplicates source path");

    const std::vector<std::string> per_entry =
        cooker.cache().probe_stale_upstream_sources(stale_pairs);
    expectTrue(per_entry.size() == 2u, "per-entry stale upstream probe returns one push per pair");

    const std::vector<std::string> unique =
        cooker.cache().probe_unique_stale_upstream_sources(stale_pairs);
    expectTrue(unique.size() == 1u, "unique stale upstream probe deduplicates source path");
    expectTrue(unique[0] == source, "unique stale upstream probe returns matching source");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 909;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    const fuse::project::CookHashPreflight zero_preflight =
        fuse::project::preflight_cook_cache_entry(zero_key);
    expectTrue(!zero_preflight.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_preflight.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
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
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would probe\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");

    writeTempFile(source, "# would probe updated\n");
    const fuse::u64 revised_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(revised_hash != seeded.content_hash, "content change alters mesh import hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, revised_hash),
               "would_invalidate_stale_content true after source content change");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would invalidate\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_output(""),
               "would_invalidate_output rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");

    writeTempFile(source, "# would invalidate revised\n");
    expectTrue(cooker.cache().would_prune_all(), "stale content makes would_prune_all true");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_key;
    invalid_key.content_hash = 0;
    invalid_key.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    invalid_key.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    const fuse::project::CookHashPreflight zero_key = fuse::project::preflight_cook_cache_entry(invalid_key);
    expectTrue(!zero_key.ok(), "zero cache key fails entry preflight");
    expectTrue(zero_key.reason == fuse::project::CookHashRejectReason::InvalidCacheKey,
               "zero cache key preflight reason is InvalidCacheKey");

    fuse::project::CookCacheEntry empty_source = invalid_key;
    empty_source.content_hash = 101;
    empty_source.source_path = "";
    const fuse::project::CookHashPreflight empty_src = fuse::project::preflight_cook_cache_entry(empty_source);
    expectTrue(!empty_src.ok(), "empty source path fails entry preflight");
    expectTrue(empty_src.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path preflight reason");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 202;
    valid.source_path = "/tmp/fuse_b79_entry_preflight_valid.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight_valid.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid entry passes preflight");
    expectTrue(fuse::project::is_valid_cook_cache_entry(valid), "preflight success implies valid entry");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::InvalidCacheKey)) == "invalid_cache_key",
               "reject reason label for invalid cache key");
}

void testCookCacheUniqueStaleUpstreamProbes() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry entry_a;
    entry_a.content_hash = 301;
    entry_a.upstream_hash = 10;
    entry_a.source_path = "/tmp/fuse_b79_unique_up_a.obj";
    entry_a.output_path = "/tmp/fuse_b79_unique_up_a.fusemesh";
    cache.store(entry_a);

    fuse::project::CookCacheEntry entry_b = entry_a;
    entry_b.content_hash = 302;
    entry_b.upstream_hash = 10;
    entry_b.source_path = "/tmp/fuse_b79_unique_up_b.obj";
    entry_b.output_path = "/tmp/fuse_b79_unique_up_b.fusemesh";
    cache.store(entry_b);

    const std::vector<std::pair<std::string, fuse::u64>> upstream = {
        {entry_a.source_path, 99},
        {entry_b.source_path, 99},
    };

    expectTrue(cache.probe_stale_upstream_sources(upstream).size() == 2u,
               "stale upstream probe lists one push per matching entry");
    expectTrue(cache.probe_unique_stale_upstream_sources(upstream).size() == 2u,
               "unique stale upstream probe deduplicates per source");
    expectTrue(cache.count_unique_stale_upstream_sources(upstream) == 2u,
               "unique stale upstream count matches deduplicated probe size");
    expectTrue(cache.would_invalidate_stale_upstream_hashes(upstream),
               "would_invalidate_stale_upstream true when stale entries exist");

    const std::vector<std::pair<std::string, fuse::u64>> fresh_upstream = {
        {entry_a.source_path, 10},
        {entry_b.source_path, 10},
    };
    expectTrue(!cache.would_invalidate_stale_upstream_hashes(fresh_upstream),
               "would_invalidate_stale_upstream false when hashes match");
    expectTrue(cache.count_unique_stale_upstream_sources(fresh_upstream) == 0u,
               "unique stale upstream count zero when hashes match");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
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
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false for matching hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true for mismatched hash");
    expectTrue(cooker.cache().count_by_source(source) == 1u,
               "count_by_source matches would_invalidate_source");

    writeTempFile(source, "# would mesh updated\n");
    const fuse::u64 updated_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(updated_hash != seeded.content_hash, "source change yields new content hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, updated_hash),
               "would_invalidate_stale_content true after source change");
}

void testCookCachePreflightStoreEntry() {
    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_store.obj", "# preflight store\n");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 909;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_store.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;

    fuse::project::CookCache cache;
    const fuse::project::CookHashPreflight ok = cache.preflight_store_entry(valid);
    expectTrue(ok.ok(), "readable cache entry passes store preflight");

    fuse::project::CookCacheEntry invalid = valid;
    invalid.content_hash = 0;
    expectTrue(!cache.preflight_store_entry(invalid).ok(), "zero hash fails store preflight");
    expectTrue(cache.preflight_store_entry(invalid).reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero hash store preflight reason");

    invalid = valid;
    invalid.source_path = "";
    expectTrue(!cache.preflight_store_entry(invalid).ok(), "empty source fails store preflight");

    invalid = valid;
    invalid.source_path = "/tmp/fuse_b79_missing_preflight_store.obj";
    expectTrue(!cache.preflight_store_entry(invalid).ok(), "missing source fails store preflight");
    expectTrue(cache.preflight_store_entry(invalid).reason ==
                   fuse::project::CookHashRejectReason::SourceUnreadable,
               "missing source store preflight reason");

    fuse::project::CookCacheEntry shader = valid;
    shader.kind = fuse::project::CookAssetKind::Shader;
    shader.output_path = "/tmp/fuse_b79_preflight_store.fuseshader";
    expectTrue(cache.preflight_store_entry(shader).ok(), "shader entry skips source readability preflight");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_all(), "would_invalidate_all on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv.obj", "# would invalidate\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_inv.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");
    expectTrue(cooker.cache().would_invalidate_all(), "would_invalidate_all true on populated cache");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source guards empty path");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output guards empty path");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_key;
    invalid_key.content_hash = 0;
    invalid_key.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    invalid_key.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    const fuse::project::CookHashPreflight zero_key = fuse::project::preflight_cook_cache_entry(invalid_key);
    expectTrue(!zero_key.ok(), "cache entry preflight rejects zero content hash");
    expectTrue(zero_key.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "cache entry zero hash preflight reason");

    fuse::project::CookCacheEntry empty_source;
    empty_source.content_hash = 101;
    empty_source.source_path = "";
    empty_source.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "cache entry preflight rejects empty source path");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 202;
    valid.source_path = "/tmp/fuse_b79_entry_preflight_valid.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight_valid.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");
}

void testCookHashPreflightManifestWithUpstream() {
    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_upstream.obj", "# upstream preflight\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry upstream;
    upstream.kind = fuse::project::CookAssetKind::Mesh;
    upstream.source_path = source;
    upstream.output_path = "/tmp/fuse_b79_preflight_upstream.fusemesh";
    manifest.assets.push_back(upstream);

    fuse::project::CookManifestEntry dependent = upstream;
    dependent.source_path = writeTempFile("/tmp/fuse_b79_preflight_dep.obj", "# dependent\n");
    dependent.output_path = "/tmp/fuse_b79_preflight_dep.fusemesh";
    dependent.dependencies.push_back(upstream.output_path);

    const fuse::project::CookHashPreflight with_deps =
        fuse::project::preflight_manifest_entry_with_upstream(dependent, manifest);
    expectTrue(with_deps.ok(), "manifest entry with valid upstream deps passes preflight");

    fuse::project::CookManifestEntry no_deps = dependent;
    no_deps.dependencies.clear();
    expectTrue(fuse::project::preflight_manifest_entry_with_upstream(no_deps, manifest).ok(),
               "manifest entry without deps skips upstream preflight");

    fuse::project::CookManifestEntry bad_deps = dependent;
    bad_deps.dependencies = {""};
    expectTrue(!fuse::project::preflight_manifest_entry_with_upstream(bad_deps, manifest).ok(),
               "manifest entry with empty dependency list fails upstream preflight");
}

void testCookCacheUniqueStaleUpstreamProbes() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry entry_a;
    entry_a.content_hash = 901;
    entry_a.upstream_hash = 1;
    entry_a.source_path = "/tmp/fuse_b79_unique_upstream_a.obj";
    entry_a.output_path = "/tmp/fuse_b79_unique_upstream_a.fusemesh";
    cache.store(entry_a);

    fuse::project::CookCacheEntry entry_b = entry_a;
    entry_b.content_hash = 902;
    cache.store(entry_b);

    const std::vector<std::pair<std::string, fuse::u64>> stale_pairs = {
        {entry_a.source_path, 99u},
    };

    expectTrue(cache.count_stale_upstream_hashes(stale_pairs) == 2u,
               "per-entry stale upstream count includes duplicates");
    expectTrue(cache.count_unique_stale_upstream_sources(stale_pairs) == 1u,
               "unique stale upstream count deduplicates source paths");

    const std::vector<std::string> probed = cache.probe_unique_stale_upstream_sources(stale_pairs);
    expectTrue(probed.size() == 1u, "unique stale upstream probe returns one source");
    expectTrue(probed[0] == entry_a.source_path, "unique stale upstream probe names matching source");
    expectTrue(cache.probe_unique_stale_upstream_sources({}).empty(),
               "unique stale upstream probe on empty list is guarded");
}

void testCookCacheWouldInvalidateMirrors() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_src.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_src.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_out.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would mesh\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate mirrors ok");

    expectTrue(cooker.cache().would_invalidate(seeded.content_hash),
               "would_invalidate mirrors contains for seeded hash");
    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source mirrors count_by_source");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output mirrors count_by_output");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash does not would_invalidate stale content");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash would_invalidate stale content");

    writeTempFile(source, "# would mesh updated\n");
    expectTrue(cooker.cache().would_prune_all(), "stale entry would_prune_all after content change");
}

void testCookCacheEntryPreflight() {
    fuse::project::CookCacheEntry invalid_key;
    invalid_key.content_hash = 0;
    invalid_key.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_key.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookCacheEntryPreflight zero_key =
        fuse::project::CookCache::preflight_cook_cache_entry(invalid_key);
    expectTrue(!zero_key.ok(), "zero content hash fails entry preflight");
    expectTrue(zero_key.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash preflight reason");

    fuse::project::CookCacheEntry empty_source;
    empty_source.content_hash = 42;
    empty_source.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(!fuse::project::CookCache::preflight_cook_cache_entry(empty_source).ok(),
               "empty source path fails entry preflight");
    expectTrue(fuse::project::CookCache::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path preflight reason");

    fuse::project::CookCacheEntry empty_output = empty_source;
    empty_output.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    empty_output.output_path = "";
    expectTrue(!fuse::project::CookCache::preflight_cook_cache_entry(empty_output).ok(),
               "empty output path fails entry preflight");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 99;
    valid.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::CookCache::preflight_cook_cache_entry(valid).ok(),
               "structurally valid entry passes preflight");
}

void testCookCacheProbeUniqueStaleUpstreamSources() {
    const std::string source = writeTempFile("/tmp/fuse_b79_unique_upstream.obj", "# unique upstream\n");

    fuse::project::CookCache cache;
    fuse::project::CookCacheEntry entry_a;
    entry_a.content_hash = 901;
    entry_a.upstream_hash = 1;
    entry_a.source_path = source;
    entry_a.output_path = "/tmp/fuse_b79_unique_upstream_a.fusemesh";
    cache.store(entry_a);

    fuse::project::CookCacheEntry entry_b = entry_a;
    entry_b.content_hash = 902;
    entry_b.output_path = "/tmp/fuse_b79_unique_upstream_b.fusemesh";
    cache.store(entry_b);
    expectTrue(cache.entry_count() == 2u, "two entries with same source seeded");

    const std::vector<std::pair<std::string, fuse::u64>> stale_upstream = {{source, 99u}};
    const std::vector<std::string> raw =
        cache.probe_stale_upstream_sources(stale_upstream);
    expectTrue(raw.size() == 2u, "raw stale upstream probe lists one push per matching entry");

    const std::vector<std::string> unique =
        cache.probe_unique_stale_upstream_sources(stale_upstream);
    expectTrue(unique.size() == 1u, "unique stale upstream probe deduplicates source paths");
    expectTrue(unique[0] == source, "unique stale upstream probe returns matching source");
    expectTrue(cache.would_invalidate_stale_upstream_hashes(stale_upstream),
               "would_invalidate_stale_upstream mirrors non-zero count");
}

void testCookHashPreflightCacheEntryGuards() {
    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight zero_hash =
        fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!zero_hash.ok(), "zero-hash cache entry fails preflight");
    expectTrue(zero_hash.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero-hash cache entry preflight reason");

    invalid.content_hash = 42;
    invalid.source_path = "";
    const fuse::project::CookHashPreflight empty_source =
        fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!empty_source.ok(), "empty source cache entry fails preflight");
    expectTrue(empty_source.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source cache entry preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry.obj", "# cache entry\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");

    fuse::project::CookCacheEntry shader_entry = valid;
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    shader_entry.output_path = "/tmp/fuse_b79_preflight_entry.fuseshader";
    expectTrue(fuse::project::preflight_cook_cache_entry(shader_entry).ok(),
               "shader cache entry passes structural preflight");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::InvalidCacheEntry)) == "invalid_cache_entry",
               "reject reason label for invalid cache entry");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_source.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_output.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_source.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(cache.count_stale_upstream_sources({{"/tmp/fuse_b79_would_source.obj", 1u}}) == 0u,
               "count_stale_upstream_sources on empty cache is zero");

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
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");
    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source would_invalidate guarded");
    expectTrue(!cooker.cache().would_invalidate_output(""), "empty output would_invalidate guarded");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_all(), "would_invalidate_all on empty cache is false");

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
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false for matching hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true for mismatched hash");
    expectTrue(cooker.cache().would_invalidate_all(), "populated cache would_invalidate_all is true");

    writeTempFile(source, "# would mesh updated\n");
    const fuse::u64 recomputed = fuse::project::hash_mesh_import(desc);
    expectTrue(recomputed != seeded.content_hash, "content change yields new hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, recomputed),
               "recomputed hash makes would_invalidate_stale_content true for stale entry");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_key;
    invalid_key.content_hash = 0;
    invalid_key.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    invalid_key.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    const fuse::project::CookHashPreflight zero_key = fuse::project::preflight_cook_cache_entry(invalid_key);
    expectTrue(!zero_key.ok(), "zero cache entry hash fails preflight");
    expectTrue(zero_key.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero cache entry hash preflight reason");

    fuse::project::CookCacheEntry empty_source = invalid_key;
    empty_source.content_hash = 42u;
    empty_source.source_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty cache entry source path preflight reason");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42u;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry passes preflight");
    expectTrue(fuse::project::is_valid_cook_cache_entry(valid), "preflight success implies valid cache entry");
}

void testCookHashManifestWithUpstreamPreflight() {
    const std::string upstream = writeTempFile("/tmp/fuse_b79_manifest_upstream.obj", "# upstream\n");
    const std::string dependent = writeTempFile("/tmp/fuse_b79_manifest_dependent.obj", "# dependent\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry upstream_entry;
    upstream_entry.kind = fuse::project::CookAssetKind::Mesh;
    upstream_entry.source_path = upstream;
    upstream_entry.output_path = "/tmp/fuse_b79_manifest_upstream.fusemesh";
    manifest.assets.push_back(upstream_entry);

    fuse::project::CookManifestEntry dependent_entry;
    dependent_entry.kind = fuse::project::CookAssetKind::Mesh;
    dependent_entry.source_path = dependent;
    dependent_entry.output_path = "/tmp/fuse_b79_manifest_dependent.fusemesh";
    dependent_entry.dependencies.push_back(upstream_entry.output_path);
    manifest.assets.push_back(dependent_entry);

    expectTrue(fuse::project::preflight_manifest_entry_with_upstream(dependent_entry, manifest).ok(),
               "manifest entry with upstream deps passes preflight");

    fuse::project::CookManifestEntry missing_upstream = dependent_entry;
    missing_upstream.dependencies = {"/tmp/fuse_b79_missing_upstream.fusemesh"};
    expectTrue(fuse::project::preflight_manifest_entry_with_upstream(missing_upstream, manifest).ok(),
               "missing manifest dependency does not fail upstream preflight");
}

void testCookHashPreflightImportPathsAndCacheEntry() {
    const fuse::project::CookHashPreflight empty_input =
        fuse::project::preflight_import_paths("", "/tmp/fuse_b79_import_paths_out.fusemesh");
    expectTrue(!empty_input.ok(), "empty import input path fails preflight");
    expectTrue(empty_input.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty import input path reason");

    const fuse::project::CookHashPreflight empty_output =
        fuse::project::preflight_import_paths("/tmp/fuse_b79_import_paths_in.obj", "");
    expectTrue(!empty_output.ok(), "empty import output path fails preflight");
    expectTrue(empty_output.reason == fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty import output path reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry.obj", "# cache entry\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 909;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid cache entry preflight ok");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    expectTrue(fuse::project::preflight_cook_cache_entry(zero_key).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero cache key fails entry preflight");

    fuse::project::CookCacheEntry shader_entry;
    shader_entry.content_hash = 910;
    shader_entry.source_path = "/tmp/fuse_b79_shader_preflight.obj";
    shader_entry.output_path = "/tmp/fuse_b79_shader_preflight.fuseshader";
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    expectTrue(fuse::project::preflight_cook_cache_entry(shader_entry).ok(),
               "shader cache entry passes structural preflight without source file");
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv.obj", "# would inv\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_inv.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded source");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded output");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash is not stale for would_invalidate_stale_content");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash is stale for would_invalidate_stale_content");
    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path would_invalidate is guarded");
}

void testCookCacheProbeUniqueStaleUpstreamSources() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_unique_upstream_a.obj", "# unique a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_unique_upstream_b.obj", "# unique b\n");

    fuse::project::MeshImportDesc desc_a;
    desc_a.input_path = source_a;
    desc_a.output_path = "/tmp/fuse_b79_unique_upstream_a.fusemesh";

    fuse::project::MeshImportDesc desc_b;
    desc_b.input_path = source_b;
    desc_b.output_path = "/tmp/fuse_b79_unique_upstream_b.fusemesh";

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_mesh(desc_a).ok, "seed cook a for unique upstream probe");
    expectTrue(cooker.cook_mesh(desc_b).ok, "seed cook b for unique upstream probe");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries for unique upstream probe");

    const std::vector<std::pair<std::string, fuse::u64>> stale_pairs = {
        {source_a, 1u},
        {source_a, 1u},
        {source_b, 2u},
    };
    const std::vector<std::string> per_entry =
        cooker.cache().probe_stale_upstream_sources(stale_pairs);
    expectTrue(per_entry.size() == 3u, "per-entry stale upstream probe counts each match");

    const std::vector<std::string> unique =
        cooker.cache().probe_unique_stale_upstream_sources(stale_pairs);
    expectTrue(unique.size() == 2u, "unique stale upstream probe deduplicates sources");
    expectTrue(unique[0] == source_a || unique[1] == source_a, "source a in unique upstream probe");
    expectTrue(unique[0] == source_b || unique[1] == source_b, "source b in unique upstream probe");
}

void testCookHashPreflightMtimeAndDependencyGuards() {
    const fuse::project::CookHashPreflight empty_mtime = fuse::project::preflight_file_mtime_ns("");
    expectTrue(!empty_mtime.ok(), "empty path fails mtime preflight");
    expectTrue(empty_mtime.reason == fuse::project::CookHashRejectReason::EmptyPath,
               "empty path mtime preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_mtime.obj", "# mtime preflight\n");
    expectTrue(fuse::project::preflight_file_mtime_ns(source).ok(), "readable path passes mtime preflight");

    const fuse::project::CookHashPreflight zero_combine = fuse::project::preflight_fnv1a64_combine(0, 42u);
    expectTrue(!zero_combine.ok(), "zero left operand fails combine preflight");
    expectTrue(zero_combine.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero left combine preflight reason");
    expectTrue(fuse::project::preflight_fnv1a64_combine(99u, 0).ok(), "non-zero left passes combine preflight");

    fuse::project::CookManifest manifest;
    expectTrue(!fuse::project::preflight_upstream_dependency_path("", manifest).ok(),
               "empty dependency path fails preflight");
    expectTrue(fuse::project::preflight_upstream_dependency_path("/tmp/fuse_b79_missing_dep.fusemesh", manifest)
                       .reason == fuse::project::CookHashRejectReason::UnresolvedDependency,
               "unknown dependency path fails preflight");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_preflight_dep.fusemesh";
    manifest.assets.push_back(entry);

    expectTrue(fuse::project::preflight_upstream_dependency_path(entry.output_path, manifest).ok(),
               "resolved dependency path passes preflight");
    expectTrue(fuse::project::preflight_upstream_dependencies_hash({entry.output_path}, manifest).ok(),
               "upstream list preflight resolves manifest dependency");

    expectTrue(std::string(fuse::project::cookHashRejectReasonLabel(
                   fuse::project::CookHashRejectReason::UnresolvedDependency)) == "unresolved_dependency",
               "reject reason label for unresolved dependency");
}

void testCookCacheInvalidationEstimateGuards() {
    fuse::project::CookCache cache;
    const fuse::project::CookCacheInvalidationEstimate empty =
        cache.estimate_invalidation_removals("/tmp/fuse_b79_est_source.obj", "/tmp/fuse_b79_est_output.fusemesh",
                                             42u, {{"/tmp/fuse_b79_est_source.obj", 1u}});
    expectTrue(empty.total() == 0u, "empty cache invalidation estimate is zero");
    expectTrue(!cache.would_invalidate_any("/tmp/fuse_b79_est_source.obj"), "empty cache would_invalidate_any false");
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_est_source.obj"),
               "would_invalidate_source false on empty cache");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_est_output.fusemesh"),
               "would_invalidate_output false on empty cache");
    expectTrue(cache.count_stale_upstream_sources({{"/tmp/fuse_b79_est_source.obj", 1u}}) == 0u,
               "deduplicated stale upstream count zero on empty cache");
    expectTrue(cache.probe_stale_upstream_sources_deduplicated({{"/tmp/fuse_b79_est_source.obj", 1u}}).empty(),
               "deduplicated stale upstream probe empty on empty cache");

    const std::string source = writeTempFile("/tmp/fuse_b79_est_inv.obj", "# est inv\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_est_inv.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for invalidation estimate ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source true for seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output true for seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path guarded");
    expectTrue(!cooker.cache().would_invalidate_output(""), "empty output path guarded");

    const fuse::project::CookCacheInvalidationEstimate estimate =
        cooker.cache().estimate_invalidation_removals(source, desc.output_path, seeded.content_hash);
    expectTrue(estimate.by_source_path == 1u, "estimate counts source-path entries");
    expectTrue(estimate.by_output_path == 1u, "estimate counts output-path entries");
    expectTrue(estimate.stale_content == 0u, "matching content hash yields zero stale-content estimate");
    expectTrue(cooker.cache().would_invalidate_any(source, desc.output_path, seeded.content_hash),
               "would_invalidate_any true for populated cache");

    writeTempFile(source, "# est inv revised\n");
    const fuse::u64 revised_hash = seeded.content_hash + 1u;
    const fuse::project::CookCacheInvalidationEstimate stale =
        cooker.cache().estimate_invalidation_removals(source, "", revised_hash);
    expectTrue(stale.stale_content == 1u, "stale content estimate reports mismatched hash");
}

void testCookCacheWouldInvalidateProbes() {
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
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash does not trigger stale-content would_invalidate");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash triggers stale-content would_invalidate");
    expectTrue(!cooker.cache().would_invalidate_source(""), "empty source path would_invalidate is guarded");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_hash;
    invalid_hash.content_hash = 0;
    invalid_hash.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    invalid_hash.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    const fuse::project::CookHashPreflight zero_hash =
        fuse::project::preflight_cook_cache_entry(invalid_hash);
    expectTrue(!zero_hash.ok(), "zero content hash fails entry preflight");
    expectTrue(zero_hash.should_skip(), "zero content hash entry preflight should_skip");
    expectTrue(zero_hash.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash entry preflight reason");

    fuse::project::CookCacheEntry empty_source;
    empty_source.content_hash = 42;
    empty_source.source_path = "";
    empty_source.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path entry preflight reason");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    const fuse::project::CookHashPreflight valid_preflight = fuse::project::preflight_cook_cache_entry(valid);
    expectTrue(valid_preflight.ok(), "valid entry passes preflight");
    expectTrue(!valid_preflight.should_skip(), "valid entry preflight does not should_skip");
    expectTrue(fuse::project::is_valid_cook_cache_entry(valid), "preflight success matches entry validation");

    expectTrue(!fuse::project::preflight_cacheable_cook_cache_key(0, 42u).ok(),
               "cacheable fold preflight rejects zero source");
    expectTrue(fuse::project::preflight_cacheable_cook_cache_key(99u, 0).ok(),
               "cacheable fold preflight allows zero upstream");
    expectTrue(fuse::project::preflight_cacheable_cook_cache_key(99u, 42u).ok(),
               "cacheable fold preflight ok for valid fold");
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

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would probe\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would-invalidation probes ok");

    expectTrue(cooker.cache().would_invalidate(seeded.content_hash), "would_invalidate reports seeded hash");
    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(!cooker.cache().would_invalidate_stale_upstream_hashes({{source, 0u}}),
               "would_invalidate_stale_upstream false when upstream matches");

    writeTempFile(source, "# would probe updated\n");
    const fuse::u64 revised_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(revised_hash != seeded.content_hash, "source change yields new content hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, revised_hash),
               "would_invalidate_stale_content true when current hash differs from stored key");
    expectTrue(cooker.cache().would_prune_all(), "would_prune_all true when stale entry present");

    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_would_invalid.obj";
    invalid.output_path = "/tmp/fuse_b79_would_invalid.fusemesh";
    const fuse::project::CookHashPreflight invalid_preflight =
        fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!invalid_preflight.ok(), "preflight_cook_cache_entry rejects zero hash");
    expectTrue(invalid_preflight.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero hash cache entry preflight reason");

    fuse::project::CookCacheEntry valid;
    valid.content_hash = seeded.content_hash;
    valid.source_path = source;
    valid.output_path = desc.output_path;
    const fuse::project::CookHashPreflight valid_preflight =
        fuse::project::preflight_cook_cache_entry(valid);
    expectTrue(valid_preflight.ok(), "preflight_cook_cache_entry accepts valid entry");
}

void testCookCacheWouldInvalidateSourceOutputProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_probe.obj", "# would probe\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_probe.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source reports seeded entry");
    expectTrue(!cache.would_invalidate_source(""), "empty source path would_invalidate is guarded");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_output(""),
               "empty output path would_invalidate is guarded");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash would_invalidate_stale_content is false");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash would_invalidate_stale_content is true");
}

void testCookCacheProbeStaleUpstreamSourcesUnique() {
    const std::string source = writeTempFile("/tmp/fuse_b79_upstream_unique.obj", "# upstream unique\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_upstream_unique.fusemesh";
    manifest.assets.push_back(entry);

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_entry(entry, manifest).ok, "seed cook for unique upstream probe");

    const fuse::u64 stale_upstream = 999u;
    const std::vector<std::pair<std::string, fuse::u64>> pairs = {{source, stale_upstream},
                                                                  {source, stale_upstream}};

    const std::vector<std::string> duplicated = cooker.cache().probe_stale_upstream_sources(pairs);
    expectTrue(duplicated.size() == 2u, "non-unique upstream probe may duplicate per entry");

    const std::vector<std::string> unique = cooker.cache().probe_stale_upstream_sources_unique(pairs);
    expectTrue(unique.size() == 1u, "unique upstream probe deduplicates source paths");
    expectTrue(unique[0] == source, "unique upstream probe returns stale source");
    expectTrue(cooker.cache().probe_stale_upstream_sources_unique({}).empty(),
               "empty upstream pair list yields empty unique probe");
}

void testCookHashShaderAndManifestUpstreamPreflights() {
    fuse::project::CookManifestEntry shader;
    shader.kind = fuse::project::CookAssetKind::Shader;
    shader.source_path = "/tmp/fuse_b79_shader_preflight.glsl";
    shader.output_path = "/tmp/fuse_b79_shader_preflight.fuseshader";
    expectTrue(fuse::project::preflight_shader_entry_hash(shader).ok(),
               "shader entry with paths passes path-only preflight");

    shader.source_path = "";
    expectTrue(fuse::project::preflight_shader_entry_hash(shader).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty shader input path preflight reason");

    const std::string dep = writeTempFile("/tmp/fuse_b79_manifest_upstream_dep.obj", "# upstream dep\n");
    const std::string consumer = writeTempFile("/tmp/fuse_b79_manifest_upstream_consumer.obj", "# consumer\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry producer;
    producer.kind = fuse::project::CookAssetKind::Mesh;
    producer.source_path = dep;
    producer.output_path = "/tmp/fuse_b79_manifest_upstream_dep.fusemesh";
    manifest.assets.push_back(producer);

    fuse::project::CookManifestEntry dependent;
    dependent.kind = fuse::project::CookAssetKind::Mesh;
    dependent.source_path = consumer;
    dependent.output_path = "/tmp/fuse_b79_manifest_upstream_consumer.fusemesh";
    dependent.dependencies.push_back(producer.output_path);
    manifest.assets.push_back(dependent);

    const fuse::project::CookHashPreflight with_upstream =
        fuse::project::preflight_manifest_entry_with_upstream(dependent, manifest);
    expectTrue(with_upstream.ok(), "manifest entry with upstream deps passes combined preflight");

    fuse::project::CookManifestEntry no_deps = dependent;
    no_deps.dependencies.clear();
    expectTrue(fuse::project::preflight_manifest_entry_with_upstream(no_deps, manifest).ok(),
               "manifest entry without deps skips upstream preflight");
}

void testCookCacheWouldInvalidateSourceOutputProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_probe.obj", "# would probe\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_probe.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source),
               "would_invalidate_source reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""),
               "empty source path would_invalidate is guarded");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_output(""),
               "empty output path would_invalidate is guarded");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash would_invalidate_stale_content is false");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash would_invalidate_stale_content is true");
}

void testCookCacheProbeStaleUpstreamSourcesUnique() {
    const std::string source = writeTempFile("/tmp/fuse_b79_upstream_unique.obj", "# upstream unique\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_upstream_unique.fusemesh";
    manifest.assets.push_back(entry);

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_entry(entry, manifest).ok, "seed cook for unique upstream probe");

    const fuse::u64 stale_upstream = 999u;
    const std::vector<std::pair<std::string, fuse::u64>> pairs = {{source, stale_upstream},
                                                                  {source, stale_upstream}};

    const std::vector<std::string> duplicated = cooker.cache().probe_stale_upstream_sources(pairs);
    expectTrue(duplicated.size() == 2u, "non-unique upstream probe may duplicate per entry");

    const std::vector<std::string> unique = cooker.cache().probe_stale_upstream_sources_unique(pairs);
    expectTrue(unique.size() == 1u, "unique upstream probe deduplicates source paths");
    expectTrue(unique[0] == source, "unique upstream probe returns stale source");
    expectTrue(cooker.cache().probe_stale_upstream_sources_unique({}).empty(),
               "empty upstream pair list yields empty unique probe");

    expectTrue(cooker.cache().would_invalidate_stale_upstream_hashes(pairs),
               "would_invalidate_stale_upstream reports stale entries");
}

void testCookHashShaderAndManifestUpstreamPreflights() {
    fuse::project::CookManifestEntry shader;
    shader.kind = fuse::project::CookAssetKind::Shader;
    shader.source_path = "/tmp/fuse_b79_shader_preflight.glsl";
    shader.output_path = "/tmp/fuse_b79_shader_preflight.fuseshader";
    expectTrue(fuse::project::preflight_shader_entry_hash(shader).ok(),
               "shader entry with paths passes path-only preflight");

    shader.source_path = "";
    expectTrue(fuse::project::preflight_shader_entry_hash(shader).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty shader input path preflight reason");

    const std::string dep = writeTempFile("/tmp/fuse_b79_manifest_upstream_dep.obj", "# upstream dep\n");
    const std::string consumer = writeTempFile("/tmp/fuse_b79_manifest_upstream_consumer.obj", "# consumer\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry producer;
    producer.kind = fuse::project::CookAssetKind::Mesh;
    producer.source_path = dep;
    producer.output_path = "/tmp/fuse_b79_manifest_upstream_dep.fusemesh";
    manifest.assets.push_back(producer);

    fuse::project::CookManifestEntry dependent;
    dependent.kind = fuse::project::CookAssetKind::Mesh;
    dependent.source_path = consumer;
    dependent.output_path = "/tmp/fuse_b79_manifest_upstream_consumer.fusemesh";
    dependent.dependencies.push_back(producer.output_path);
    manifest.assets.push_back(dependent);

    const fuse::project::CookHashPreflight with_upstream =
        fuse::project::preflight_manifest_entry_with_upstream(dependent, manifest);
    expectTrue(with_upstream.ok(), "manifest entry with upstream deps passes combined preflight");

    fuse::project::CookManifestEntry no_deps = dependent;
    no_deps.dependencies.clear();
    expectTrue(fuse::project::preflight_manifest_entry_with_upstream(no_deps, manifest).ok(),
               "manifest entry without deps skips upstream preflight");
}

void testCookHashCacheEntryPreflight() {
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(),
               "valid cache entry passes entry preflight");

    fuse::project::CookCacheEntry invalid = valid;
    invalid.content_hash = 0;
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid).reason ==
                   fuse::project::CookHashRejectReason::ZeroContentHash,
               "zero content hash entry preflight reason");

    invalid = valid;
    invalid.source_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path entry preflight reason");
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
    expectTrue(cache.probe_stale_upstream_source_paths({{"/tmp/fuse_b79_would_up.obj", 1u}}).empty(),
               "probe_stale_upstream_source_paths on empty cache is empty");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would probe\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");

    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output rejects empty path");
}

void testCookCachePreflightStoreEntry() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry invalid_key;
    invalid_key.content_hash = 0;
    invalid_key.source_path = "/tmp/fuse_b79_preflight_store.obj";
    invalid_key.output_path = "/tmp/fuse_b79_preflight_store.fusemesh";
    const fuse::project::CookHashPreflight zero_key = cache.preflight_store_entry(invalid_key);
    expectTrue(!zero_key.ok(), "zero content hash fails store preflight");
    expectTrue(zero_key.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash store preflight reason");

    fuse::project::CookCacheEntry empty_source;
    empty_source.content_hash = 42;
    empty_source.output_path = "/tmp/fuse_b79_preflight_store.fusemesh";
    const fuse::project::CookHashPreflight empty_src = cache.preflight_store_entry(empty_source);
    expectTrue(!empty_src.ok(), "empty source path fails store preflight");
    expectTrue(empty_src.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source store preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_store_ok.obj", "# store preflight\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 99;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_store_ok.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    const fuse::project::CookHashPreflight ok = cache.preflight_store_entry(valid);
    expectTrue(ok.ok(), "readable mesh entry passes store preflight");

    fuse::project::CookCacheEntry shader;
    shader.content_hash = 100;
    shader.source_path = "/tmp/fuse_b79_preflight_shader.obj";
    shader.output_path = "/tmp/fuse_b79_preflight_shader.fuseshader";
    shader.kind = fuse::project::CookAssetKind::Shader;
    const fuse::project::CookHashPreflight shader_preflight = cache.preflight_store_entry(shader);
    expectTrue(!shader_preflight.ok(), "shader kind fails store preflight");
    expectTrue(shader_preflight.reason == fuse::project::CookHashRejectReason::SourceUnreadable,
               "shader store preflight reason");
}

void testCookCacheProbeStaleUpstreamSourcePaths() {
    fuse::project::CookCache cache;

    fuse::project::CookCacheEntry fresh;
    fresh.content_hash = 201;
    fresh.upstream_hash = 10;
    fresh.source_path = "/tmp/fuse_b79_up_probe_a.obj";
    fresh.output_path = "/tmp/fuse_b79_up_probe_a.fusemesh";
    cache.store(fresh);

    fuse::project::CookCacheEntry stale;
    stale.content_hash = 202;
    stale.upstream_hash = 20;
    stale.source_path = "/tmp/fuse_b79_up_probe_b.obj";
    stale.output_path = "/tmp/fuse_b79_up_probe_b.fusemesh";
    cache.store(stale);

    fuse::project::CookCacheEntry stale_dup = stale;
    stale_dup.content_hash = 203;
    cache.store(stale_dup);
    expectTrue(cache.entry_count() == 3u, "three entries seeded for upstream source path probe");

    const std::vector<std::pair<std::string, fuse::u64>> fresh_upstream = {
        {"/tmp/fuse_b79_up_probe_a.obj", 10u},
        {"/tmp/fuse_b79_up_probe_b.obj", 20u},
    };
    expectTrue(cache.probe_stale_upstream_source_paths(fresh_upstream).empty(),
               "matching upstream hashes yield empty deduplicated probe");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes(fresh_upstream),
               "would_invalidate_stale_upstream false when hashes match");

    const std::vector<std::pair<std::string, fuse::u64>> stale_upstream = {
        {"/tmp/fuse_b79_up_probe_a.obj", 10u},
        {"/tmp/fuse_b79_up_probe_b.obj", 99u},
    };

    const std::vector<std::string> per_entry = cache.probe_stale_upstream_sources(stale_upstream);
    expectTrue(per_entry.size() == 2u, "per-entry stale upstream probe reports one push per stale entry");

    const std::vector<std::string> deduped = cache.probe_stale_upstream_source_paths(stale_upstream);
    expectTrue(deduped.size() == 1u, "deduplicated stale upstream source path probe");
    expectTrue(deduped[0] == "/tmp/fuse_b79_up_probe_b.obj", "deduplicated stale upstream source path");
    expectTrue(cache.would_invalidate_stale_upstream_hashes(stale_upstream),
               "would_invalidate_stale_upstream true when stale entries present");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({}), "empty upstream list guarded");
}

void testCookCacheWouldInvalidateProbes() {
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

    expectTrue(!cache.would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(!cache.would_invalidate_output(""), "would_invalidate_output rejects empty path");
    expectTrue(!cache.would_invalidate_stale_content_for_source("", 42u),
               "would_invalidate_stale_content rejects empty source path");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_guard.obj", 0u),
               "would_invalidate_stale_content rejects zero content hash");

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
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");

    writeTempFile(source, "# would mesh updated\n");
    const fuse::u64 updated_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(updated_hash != seeded.content_hash, "source change yields new content hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, updated_hash),
               "would_invalidate_stale_content true after source change with fresh hash");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight zero_hash = fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!zero_hash.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_hash.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash preflight reason");

    invalid.content_hash = 501;
    invalid.source_path = "";
    const fuse::project::CookHashPreflight empty_source = fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!empty_source.ok(), "empty source path fails cache entry preflight");
    expectTrue(empty_source.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path preflight reason");

    invalid.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid.output_path = "";
    const fuse::project::CookHashPreflight empty_output = fuse::project::preflight_cook_cache_entry(invalid);
    expectTrue(!empty_output.ok(), "empty output path fails cache entry preflight");
    expectTrue(empty_output.reason == fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output path preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry_ok.obj", "# preflight entry\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 502;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry_ok.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    const fuse::project::CookHashPreflight readable = fuse::project::preflight_cook_cache_entry(valid);
    expectTrue(readable.ok(), "readable mesh cache entry passes preflight");

    fuse::project::CookCache cache;
    cache.store(valid);
    expectTrue(cache.entry_count() == 1u, "preflight-valid entry is stored");

    fuse::project::CookCacheEntry unreadable = valid;
    unreadable.content_hash = 503;
    unreadable.source_path = "/tmp/fuse_b79_preflight_entry_missing.obj";
    expectTrue(!fuse::project::preflight_cook_cache_entry(unreadable).ok(),
               "missing source fails cache entry preflight");
    cache.store(unreadable);
    expectTrue(cache.entry_count() == 2u,
               "store still accepts structurally valid entry when preflight would fail");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 42;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    const fuse::project::CookCacheEntryPreflight valid_preflight =
        fuse::project::preflight_cook_cache_entry(valid);
    expectTrue(valid_preflight.ok(), "valid cache entry passes store preflight");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    const fuse::project::CookCacheEntryPreflight zero_preflight =
        fuse::project::preflight_cook_cache_entry(zero_key);
    expectTrue(!zero_preflight.ok(), "zero content hash fails store preflight");
    expectTrue(zero_preflight.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
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
}

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_down.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would invalidate\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "matching hash does not would_invalidate stale content");

    writeTempFile(source, "# would invalidate revised\n");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "mismatched hash would_invalidate stale content");
    expectTrue(cooker.cache().would_prune_all(), "stale content makes would_prune_all true");
}

void testCookCacheStaleUpstreamUniqueProbe() {
    const std::string source = writeTempFile("/tmp/fuse_b79_unique_up.obj", "# unique upstream\n");

    fuse::project::CookCache cache;
    fuse::project::CookCacheEntry first;
    first.content_hash = 901;
    first.upstream_hash = 1;
    first.source_path = source;
    first.output_path = "/tmp/fuse_b79_unique_up_a.fusemesh";
    cache.store(first);

    fuse::project::CookCacheEntry second = first;
    second.content_hash = 902;
    second.output_path = "/tmp/fuse_b79_unique_up_b.fusemesh";
    cache.store(second);
    expectTrue(cache.entry_count() == 2u, "two entries with same source seeded");

    const std::vector<std::pair<std::string, fuse::u64>> stale_upstream = {{source, 99u}};
    const std::vector<std::string> raw = cache.probe_stale_upstream_sources(stale_upstream);
    expectTrue(raw.size() == 2u, "raw stale upstream probe lists one entry per match");

    const std::vector<std::string> unique = cache.probe_stale_upstream_sources_unique(stale_upstream);
    expectTrue(unique.size() == 1u, "unique stale upstream probe deduplicates source paths");
    expectTrue(unique[0] == source, "unique stale upstream probe preserves source path");
    expectTrue(cache.would_invalidate_stale_upstream_hashes(stale_upstream),
               "would_invalidate_stale_upstream reports stale entries");
}

void testCookHashImportCacheKeyPreflight() {
    const std::string source = writeTempFile("/tmp/fuse_b79_import_key.obj", "# import key\n");

    fuse::project::MeshImportDesc mesh;
    mesh.input_path = source;
    mesh.output_path = "/tmp/fuse_b79_import_key.fusemesh";
    const fuse::project::CookHashPreflight mesh_key =
        fuse::project::preflight_mesh_import_cache_key(mesh, 42u);
    expectTrue(mesh_key.ok(), "mesh import cache key preflight ok");
    expectTrue(fuse::project::is_valid_cook_hash_preflight(mesh_key), "mesh import cache key helper agrees");
    expectTrue(fuse::project::preflight_mesh_import_cache_key(mesh, 0).ok(),
               "mesh import cache key preflight allows zero upstream");

    mesh.input_path = "";
    expectTrue(!fuse::project::preflight_mesh_import_cache_key(mesh).ok(),
               "empty mesh input fails import cache key preflight");

    fuse::project::TextureImportDesc tex;
    tex.input_path = source;
    tex.output_path = "/tmp/fuse_b79_import_key.fusetex";
    expectTrue(fuse::project::preflight_texture_import_cache_key(tex).ok(),
               "texture import cache key preflight ok");
    tex.output_path = "";
    expectTrue(fuse::project::preflight_texture_import_cache_key(tex).reason ==
                   fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty texture output fails import cache key preflight");

    fuse::project::AudioImportDesc audio;
    audio.input_path = source;
    audio.output_path = "/tmp/fuse_b79_import_key.fuseaudio";
    expectTrue(fuse::project::preflight_audio_import_cache_key(audio).ok(),
               "audio import cache key preflight ok");
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

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would probe\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");

    writeTempFile(source, "# would probe updated\n");
    const fuse::u64 refreshed_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(refreshed_hash != seeded.content_hash, "content change yields new hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, refreshed_hash),
               "would_invalidate_stale_content true when current hash differs from stored");
    expectTrue(cooker.cache().would_prune_all(), "stale content makes would_prune_all true");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_source;
    invalid_source.content_hash = 42;
    invalid_source.source_path = "";
    invalid_source.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight empty_source =
        fuse::project::preflight_cook_cache_entry(invalid_source);
    expectTrue(!empty_source.ok(), "empty source path fails cache entry preflight");
    expectTrue(empty_source.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source preflight reason");

    fuse::project::CookCacheEntry invalid_output = invalid_source;
    invalid_output.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_output.output_path = "";
    const fuse::project::CookHashPreflight empty_output =
        fuse::project::preflight_cook_cache_entry(invalid_output);
    expectTrue(!empty_output.ok(), "empty output path fails cache entry preflight");
    expectTrue(empty_output.reason == fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output preflight reason");

    fuse::project::CookCacheEntry zero_key = invalid_output;
    zero_key.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    zero_key.content_hash = 0;
    const fuse::project::CookHashPreflight zero_hash =
        fuse::project::preflight_cook_cache_entry(zero_key);
    expectTrue(!zero_hash.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_hash.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero hash preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry.obj", "# entry preflight\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 99;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "readable entry passes preflight");

    fuse::project::CookCacheEntry shader_entry = valid;
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    shader_entry.source_path = "/tmp/fuse_b79_missing_shader_entry.obj";
    expectTrue(fuse::project::preflight_cook_cache_entry(shader_entry).ok(),
               "shader entry preflight skips unreadable source");
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

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would probe\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");

    writeTempFile(source, "# would probe updated\n");
    const fuse::u64 refreshed_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(refreshed_hash != seeded.content_hash, "content change yields new hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, refreshed_hash),
               "would_invalidate_stale_content true when current hash differs from stored");
    expectTrue(cooker.cache().would_prune_all(), "stale content makes would_prune_all true");
}

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid_source;
    invalid_source.content_hash = 42;
    invalid_source.source_path = "";
    invalid_source.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    const fuse::project::CookHashPreflight empty_source =
        fuse::project::preflight_cook_cache_entry(invalid_source);
    expectTrue(!empty_source.ok(), "empty source path fails cache entry preflight");
    expectTrue(empty_source.reason == fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source preflight reason");

    fuse::project::CookCacheEntry invalid_output = invalid_source;
    invalid_output.source_path = "/tmp/fuse_b79_preflight_entry.obj";
    invalid_output.output_path = "";
    const fuse::project::CookHashPreflight empty_output =
        fuse::project::preflight_cook_cache_entry(invalid_output);
    expectTrue(!empty_output.ok(), "empty output path fails cache entry preflight");
    expectTrue(empty_output.reason == fuse::project::CookHashRejectReason::EmptyOutputPath,
               "empty output preflight reason");

    fuse::project::CookCacheEntry zero_key = invalid_output;
    zero_key.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    zero_key.content_hash = 0;
    const fuse::project::CookHashPreflight zero_hash =
        fuse::project::preflight_cook_cache_entry(zero_key);
    expectTrue(!zero_hash.ok(), "zero content hash fails cache entry preflight");
    expectTrue(zero_hash.reason == fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero hash preflight reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_entry.obj", "# entry preflight\n");
    fuse::project::CookCacheEntry valid;
    valid.content_hash = 99;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_preflight_entry.fusemesh";
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "readable entry passes preflight");

    fuse::project::CookCacheEntry shader_entry = valid;
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    shader_entry.source_path = "/tmp/fuse_b79_missing_shader_entry.obj";
    expectTrue(fuse::project::preflight_cook_cache_entry(shader_entry).ok(),
               "shader entry preflight skips unreadable source");
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

void testCookCacheLoadPreservesEntriesWithoutOnDiskSource() {
    const std::string cachePath = "/tmp/fuse_b79_missing_source_load.json";
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

void testCookCachePruneEstimateAndProbes() {
    fuse::project::CookCache empty;
    expectTrue(empty.estimate_prunable_entries() == 0u, "empty cache prune estimate is zero");
    expectTrue(!empty.estimate_prune_all().would_prune(), "empty cache prune estimate would not prune");
    expectTrue(!empty.probe_invalidate_source("/tmp/fuse_b79_probe.obj").would_invalidate(),
               "empty cache source probe is guarded");
void testCookHashPreflightGuards() {
    expectTrue(!fuse::project::preflight_hash_file_content("").ok, "empty path fails hash preflight");
    expectTrue(fuse::project::preflight_hash_file_content("").reject ==
                   fuse::project::CookHashPreflightReject::EmptyPath,
               "empty path preflight reports EmptyPath");

    expectTrue(!fuse::project::preflight_hash_file_content("/tmp/fuse_b79_missing_preflight.obj").ok,
               "missing file fails hash preflight");
    expectTrue(fuse::project::preflight_hash_file_content("/tmp/fuse_b79_missing_preflight.obj").reject ==
                   fuse::project::CookHashPreflightReject::MissingFile,
               "missing file preflight reports MissingFile");

    const fuse::project::CookHashPreflight ok = fuse::project::preflight_hash_file_content(source);
    expectTrue(ok.ok, "existing file passes hash preflight");
    expectTrue(ok.reject == fuse::project::CookHashPreflightReject::None, "ok preflight reject is None");

    fuse::project::MeshImportDesc desc;
    desc.input_path = "";
    desc.output_path = "/tmp/fuse_b79_preflight_mesh.fusemesh";
    expectTrue(!fuse::project::preflight_mesh_import(desc).ok, "empty mesh input fails preflight");

    desc.input_path = source;
    expectTrue(fuse::project::preflight_mesh_import(desc).ok, "valid mesh desc passes preflight");

    expectTrue(!fuse::project::preflight_cook_cache_key(0, 0).ok, "zero cache key fails preflight");
    expectTrue(fuse::project::preflight_cook_cache_key(0, 0).reject ==
                   fuse::project::CookHashPreflightReject::ZeroKey,
               "zero cache key preflight reports ZeroKey");
    expectTrue(fuse::project::preflight_cook_cache_key(99u, 0).ok, "valid source-only cache key passes preflight");

void testCookCacheInvalidationProbes() {
    fuse::project::CookCache cache;
    expectTrue(cache.count_stale_entries() == 0u, "empty cache stale probe is zero");
    expectTrue(cache.count_invalid_entries() == 0u, "empty cache invalid probe is zero");
    expectTrue(cache.estimate_prune_all() == 0u, "empty cache prune estimate is zero");
    expectTrue(cache.probe_stale_content_for_source("/tmp/fuse_b79_probe.obj", 42u) == 0u,
               "empty cache stale-content probe is zero");

    const std::vector<std::pair<std::string, fuse::u64>> empty_upstream;
    expectTrue(cache.count_stale_upstream_entries(empty_upstream) == 0u,
               "empty upstream probe on empty cache is zero");
    expectTrue(cache.count_stale_upstream_entries({{"", 1u}}) == 0u,
               "empty source path upstream probe is zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_probe_stale.obj", "# probe v1\n");
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
    fuse::project::CookHashPreflightRejectReason reason = fuse::project::CookHashPreflightRejectReason::None;

    expectTrue(!fuse::project::preflight_hash_file_content("", &reason),
               "empty path fails hash file preflight");
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::EmptyPath,
               "empty path reports EmptyPath reject reason");

    expectTrue(!fuse::project::preflight_hash_file_content("/tmp/fuse_b79_missing_preflight.obj", &reason),
               "missing file fails hash file preflight");
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::MissingFile,
               "missing file reports MissingFile reject reason");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_mesh.obj", "# preflight mesh\n");
    expectTrue(fuse::project::preflight_hash_file_content(source, &reason),
               "readable file passes hash file preflight");
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::None,
               "readable file reports None reject reason");

    fuse::project::MeshImportDesc mesh;
    mesh.input_path = "";
    mesh.output_path = "/tmp/fuse_b79_preflight_out.fusemesh";
    expectTrue(!fuse::project::preflight_hash_mesh_import(mesh, &reason),
               "empty mesh input fails import preflight");
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::EmptyInputOrOutput,
               "empty mesh input reports EmptyInputOrOutput");

    mesh.input_path = source;
    expectTrue(fuse::project::preflight_hash_mesh_import(mesh, &reason),
               "valid mesh import passes preflight");
    expectTrue(fuse::project::hash_mesh_import(mesh) != 0,
               "preflight success implies non-zero mesh hash on valid path");

    expectTrue(!fuse::project::preflight_combine_cook_cache_key(0, 42u, &reason),
               "zero source hash fails combine preflight");
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::ZeroSourceHash,
               "zero source hash reports ZeroSourceHash");
    expectTrue(fuse::project::preflight_combine_cook_cache_key(99u, 0u, &reason),
               "valid source hash passes combine preflight with zero upstream");

    fuse::project::CookManifest manifest;
    expectTrue(fuse::project::preflight_hash_upstream_dependencies({}, manifest, &reason),
               "empty dependency list passes upstream preflight");
    expectTrue(fuse::project::preflight_hash_upstream_dependencies({""}, manifest, &reason),
               "all-empty dependency paths pass upstream preflight");

    const std::string dep = writeTempFile("/tmp/fuse_b79_preflight_upstream.obj", "# upstream dep\n");
    fuse::project::CookManifestEntry asset;
    asset.kind = fuse::project::CookAssetKind::Mesh;
    asset.source_path = dep;
    asset.output_path = "/tmp/fuse_b79_preflight_upstream.fusemesh";
    manifest.assets.push_back(asset);

    expectTrue(fuse::project::preflight_hash_upstream_dependencies({asset.output_path}, manifest, &reason),
               "manifest-backed dependency passes upstream preflight");
    expectTrue(!fuse::project::preflight_hash_upstream_dependencies({"/tmp/fuse_b79_missing_upstream.fusemesh"},
                                                                    manifest, &reason),
               "unknown dependency output fails upstream preflight");
               "unknown dependency output reports MissingFile");

    const std::string source = writeTempFile("/tmp/fuse_b79_probe_mesh.obj", "# probe mesh\n");

    desc.output_path = "/tmp/fuse_b79_probe_mesh.fusemesh";


    expectTrue(seeded.ok, "seed cook for invalidation probes ok");
    expectTrue(cooker.cache().entry_count() == 1u, "cache seeded for probes");

    expectTrue(cooker.cache().probe_would_invalidate_hash(seeded.content_hash),
               "probe reports known hash would invalidate");
    expectTrue(!cooker.cache().probe_would_invalidate_hash(seeded.content_hash + 1u),
               "probe rejects unknown hash");
    expectTrue(cooker.cache().probe_would_invalidate_source(source),
               "probe reports known source would invalidate");
    expectTrue(!cooker.cache().probe_would_invalidate_source(""),
               "probe rejects empty source path");

    expectTrue(cooker.cache().estimate_invalidation_by_hash(seeded.content_hash) == 1u,
               "estimate by hash counts seeded entry");
    expectTrue(cooker.cache().estimate_invalidation_by_source(source) == 1u,
               "estimate by source counts seeded entry");
    expectTrue(cooker.cache().estimate_invalidation_by_output(desc.output_path) == 1u,
               "estimate by output counts seeded entry");
    expectTrue(cooker.cache().estimate_stale_content_invalidation(source, seeded.content_hash) == 0u,
               "matching current hash estimates zero stale-content removals");
    expectTrue(cooker.cache().estimate_stale_content_invalidation(source, seeded.content_hash + 1u) == 1u,
               "mismatched current hash estimates one stale-content removal");

    expectTrue(cooker.cache().probe_would_invalidate_output(desc.output_path),
               "probe reports known output would invalidate");
    expectTrue(!cooker.cache().probe_would_invalidate_output(""),
               "probe rejects empty output path");
    expectTrue(!cooker.cache().probe_would_invalidate_stale_content(source, seeded.content_hash),
               "matching hash probe reports no stale-content invalidation");
    expectTrue(cooker.cache().probe_would_invalidate_stale_content(source, seeded.content_hash + 1u),
               "mismatched hash probe reports stale-content invalidation");

    const fuse::u64 invalidations_before = cooker.cache().stats().invalidations;
    expectTrue(cooker.cache().entry_count() == 1u, "probes leave cache untouched");
    expectTrue(cooker.cache().stats().invalidations == invalidations_before,
               "probes do not bump invalidation stats");

void testCookCacheReconcileEstimators() {

    fuse::project::CookCacheReconcileEstimate empty_estimate = cache.estimate_reconcile();
    expectTrue(empty_estimate.invalid_entries == 0u, "empty cache estimates zero invalid entries");
    expectTrue(empty_estimate.stale_entries == 0u, "empty cache estimates zero stale entries");
    expectTrue(empty_estimate.prunable_entries == 0u, "empty cache estimates zero prunable entries");
    expectTrue(cache.estimate_prune_all() == 0u, "empty cache prune-all estimate is zero");

    const std::string valid_source = writeTempFile("/tmp/fuse_b79_reconcile_valid.obj", "# reconcile valid\n");
    const std::string stale_source = writeTempFile("/tmp/fuse_b79_reconcile_stale.obj", "# reconcile stale v1\n");

    fuse::project::MeshImportDesc valid_desc;
    valid_desc.input_path = valid_source;
    valid_desc.output_path = "/tmp/fuse_b79_reconcile_valid.fusemesh";

    fuse::project::MeshImportDesc stale_desc;
    stale_desc.input_path = stale_source;
    stale_desc.output_path = "/tmp/fuse_b79_reconcile_stale.fusemesh";

    const fuse::project::CookRecord valid_cook = cooker.cook_mesh(valid_desc);
    const fuse::project::CookRecord stale_cook = cooker.cook_mesh(stale_desc);
    expectTrue(valid_cook.ok && stale_cook.ok, "seed entries for reconcile estimators");

    writeTempFile(stale_source, "# reconcile stale v2\n");

    const fuse::project::CookCacheReconcileEstimate estimate = cooker.cache().estimate_reconcile();
    expectTrue(estimate.stale_entries == 1u, "reconcile estimate counts one stale entry");
    expectTrue(estimate.invalid_entries == 0u, "reconcile estimate counts zero invalid entries");
    expectTrue(estimate.prunable_entries == 1u, "reconcile estimate counts one prunable entry");
    expectTrue(cooker.cache().estimate_prune_all() == estimate.prunable_entries,
               "prune-all estimate matches reconcile prunable count");

    const fuse::u32 removed = cooker.cache().prune_all();
    expectTrue(removed == estimate.prunable_entries, "actual prune matches reconcile estimate");
    expectTrue(cooker.cache().estimate_prune_all() == 0u, "clean cache has zero prune estimate");
    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "seed cook for invalidation probes ok");
    expectTrue(cooker.cache().count_stale_entries() == 0u, "fresh entry is not stale");
    expectTrue(cooker.cache().count_invalid_entries() == 0u, "fresh entry is not invalid");
    expectTrue(cooker.cache().estimate_prune_all() == 0u, "fresh cache prune estimate is zero");
    expectTrue(cooker.cache().probe_stale_content_for_source(source, first.content_hash) == 0u,
               "matching content hash probe is zero");

    expectTrue(cooker.cache().count_stale_entries() == 1u, "stale probe counts changed source");
    expectTrue(cooker.cache().has_prunable_entries(), "stale probe agrees with has_prunable_entries");
    expectTrue(cooker.cache().estimate_prune_all() == 1u, "prune estimate matches stale count");
    expectTrue(cooker.cache().probe_stale_content_for_source(source, first.content_hash + 1u) == 1u,
               "mismatched content hash probe counts stale entry");
    expectTrue(cooker.cache().probe_stale_content_for_source(source, 0u) == 0u,
               "zero current hash stale-content probe is zero");

    expectTrue(removed == 1u, "prune_all removes probed stale entry");
    expectTrue(cooker.cache().estimate_prune_all() == 0u, "clean cache prune estimate is zero");

void testCookCacheReconcileEstimatorMatchesPrune() {
    const std::string valid_source = writeTempFile("/tmp/fuse_b79_est_valid.obj", "# estimate valid\n");
    const std::string stale_source = writeTempFile("/tmp/fuse_b79_est_stale.obj", "# estimate v1\n");

    valid_desc.output_path = "/tmp/fuse_b79_est_valid.fusemesh";

    stale_desc.output_path = "/tmp/fuse_b79_est_stale.fusemesh";

    expectTrue(cooker.cook_mesh(valid_desc).ok, "seed valid entry for reconcile estimate");
    expectTrue(cooker.cook_mesh(stale_desc).ok, "seed stale-tracked entry for reconcile estimate");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries before stale change");

    writeTempFile(stale_source, "# estimate v2\n");
    const fuse::u32 estimated = cooker.cache().estimate_prune_all();
    expectTrue(estimated == 1u, "reconcile estimator counts one stale entry");
    expectTrue(estimated == cooker.cache().count_stale_entries(),
               "estimate_prune_all matches count_stale_entries");
    expectTrue(cooker.cache().count_invalid_entries() == 0u, "mixed cache has no invalid entries");

    expectTrue(removed == estimated, "prune_all removal matches reconcile estimate");

void testCookCacheStaleUpstreamProbe() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_probe_a.obj", "# probe upstream a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_probe_b.obj", "# probe upstream b\n");


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

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook seeds upstream probe cache");
    expectTrue(cooker.estimate_stale_dependency_reconcile(manifest) == 0u,
               "fresh manifest reconcile estimate is zero");

    writeTempFile(sourceA, "# probe upstream a revised\n");
    const fuse::u32 estimated = cooker.estimate_stale_dependency_reconcile(manifest);
    expectTrue(estimated >= 1u, "upstream change raises reconcile estimate");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= estimated, "dependency reconcile removes at least estimated upstream stale entries");
               "reconcile estimate is zero after dependency reconcile");
void testCookCacheWouldInvalidateProbes() {
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_src.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_out.fusemesh"),
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
               "would_invalidate_output on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_stale.obj", 42u),
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_up.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_invalidate_downstream_of("/tmp/fuse_b79_would_down.fusemesh", {}, {}),
               "would_invalidate_downstream on empty cache is false");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv_mesh.obj", "# would inv\n");
    desc.output_path = "/tmp/fuse_b79_would_inv_mesh.fusemesh";

void testCookCacheEntryPreflightGuards() {
    fuse::project::CookCacheEntry invalid;
    invalid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    invalid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    expectTrue(!fuse::project::preflight_cook_cache_entry(invalid).ok(),
               "zero content hash fails cache entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero content hash preflight reason");

    invalid.content_hash = 42;
    invalid.source_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(invalid).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source path fails cache entry preflight");

    const std::string source = writeTempFile("/tmp/fuse_b79_entry_preflight_ok.obj", "# entry preflight\n");
    valid.content_hash = 42;
    fuse::project::CookCacheEntry valid;
    valid.source_path = source;
    valid.output_path = "/tmp/fuse_b79_entry_preflight_ok.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(),
               "valid mesh cache entry passes preflight");

    valid.kind = fuse::project::CookAssetKind::Shader;
               "shader cache entry passes structural preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(),
}

void testCookHashPreflightManifestCookKey() {
    const std::string source = writeTempFile("/tmp/fuse_b79_manifest_key.obj", "# manifest key\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_manifest_key.fusemesh";
    manifest.assets.push_back(entry);

    expectTrue(fuse::project::preflight_manifest_cook_key(entry, manifest).ok(),
               "manifest cook key preflight ok for readable source");

    entry.source_path = "";
    expectTrue(!fuse::project::preflight_manifest_cook_key(entry, manifest).ok(),
               "empty source fails manifest cook key preflight");

    entry.source_path = source;
    entry.dependencies = {""};
    const fuse::project::CookHashPreflight all_empty_deps =
        fuse::project::preflight_manifest_cook_key(entry, manifest);
    expectTrue(!all_empty_deps.ok(), "all-empty dependency list fails manifest cook key preflight");
    expectTrue(all_empty_deps.reason == fuse::project::CookHashRejectReason::EmptyDependencyList,
               "all-empty dependency list preflight reason");

    expectTrue(!cache.would_invalidate_stale_content_for_source("/tmp/fuse_b79_would_source.obj", 42u),
    expectTrue(cache.probe_invalidation_hashes_for_source("/tmp/fuse_b79_would_source.obj").empty(),
               "probe_invalidation_hashes on empty cache is empty");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would mesh\n");
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";

}

void testCookHashPreflightImportCookKeys() {
    const std::string source = writeTempFile("/tmp/fuse_b79_import_cook_key.obj", "# import cook key\n");

    fuse::project::MeshImportDesc mesh;
    mesh.input_path = source;
    mesh.output_path = "/tmp/fuse_b79_import_cook_key.fusemesh";
    expectTrue(fuse::project::preflight_mesh_import_cook_key(mesh).ok(),
               "mesh import cook key preflight ok");
    expectTrue(fuse::project::preflight_mesh_import_cook_key(mesh, 42u).ok(),
               "mesh import cook key preflight ok with upstream");

    fuse::project::TextureImportDesc tex;
    tex.input_path = source;
    tex.output_path = "/tmp/fuse_b79_import_cook_key.fusetex";
    expectTrue(fuse::project::preflight_texture_import_cook_key(tex).ok(),
               "texture import cook key preflight ok");

    fuse::project::AudioImportDesc audio;
    audio.input_path = source;
    audio.output_path = "/tmp/fuse_b79_import_cook_key.fuseaudio";
    expectTrue(fuse::project::preflight_audio_import_cook_key(audio).ok(),
               "audio import cook key preflight ok");

    mesh.input_path = "";
    expectTrue(!fuse::project::preflight_mesh_import_cook_key(mesh).ok(),
               "empty mesh input fails import cook key preflight");

void testCookCacheWouldInvalidateProbes() {
    fuse::project::CookCache cache;
    expectTrue(!cache.would_invalidate_source("/tmp/fuse_b79_would_source.obj"),
               "would_invalidate_source on empty cache is false");
    expectTrue(!cache.would_invalidate_output("/tmp/fuse_b79_would_output.fusemesh"),
               "would_invalidate_output on empty cache is false");
               "would_invalidate_stale_content on empty cache is false");
    expectTrue(!cache.would_invalidate_stale_upstream_hashes({{"/tmp/fuse_b79_would_source.obj", 1u}}),
               "would_invalidate_stale_upstream on empty cache is false");
    expectTrue(!cache.would_prune_invalid(), "would_prune_invalid on empty cache is false");
    expectTrue(!cache.would_prune_stale(), "would_prune_stale on empty cache is false");
               "probe_invalidation_hashes for source on empty cache is empty");
    expectTrue(cache.probe_invalidation_hashes_for_output("/tmp/fuse_b79_would_output.fusemesh").empty(),
               "probe_invalidation_hashes for output on empty cache is empty");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate probes ok");

    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source rejects empty path");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path),
               "would_invalidate_output reports seeded entry");
    expectTrue(!cooker.cache().would_invalidate_output(""), "would_invalidate_output rejects empty path");
    const fuse::project::CookCacheInvalidationSurface empty_surface = cache.estimate_invalidation_surface();
    expectTrue(empty_surface.entry_count == 0u, "empty cache invalidation surface entry count is zero");
    expectTrue(empty_surface.reconcile_total() == 0u, "empty cache invalidation surface reconcile is zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_mesh.obj", "# would mesh\n");
    desc.output_path = "/tmp/fuse_b79_would_mesh.fusemesh";


void testCookCacheEntryPreflightGuards() {
    valid.content_hash = 901;
    valid.source_path = "/tmp/fuse_b79_entry_preflight.obj";
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";

    const fuse::project::CookCacheEntryPreflight ok = fuse::project::preflight_cook_cache_entry(valid);
    expectTrue(ok.ok(), "valid cache entry passes store preflight");
    expectTrue(!ok.should_skip(), "valid cache entry should not skip store");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    const fuse::project::CookCacheEntryPreflight zero_preflight =
        fuse::project::preflight_cook_cache_entry(zero_key);
    expectTrue(!zero_preflight.ok(), "zero content hash fails store preflight");
    expectTrue(zero_preflight.zero_content_hash, "zero content hash flag set");
    expectTrue(zero_preflight.should_skip(), "zero content hash should skip store");

    fuse::project::CookCacheEntry empty_source = valid;
    empty_source.source_path = "";
    const fuse::project::CookCacheEntryPreflight empty_source_preflight =
        fuse::project::preflight_cook_cache_entry(empty_source);
    expectTrue(empty_source_preflight.empty_source_path, "empty source path flag set");
    expectTrue(empty_source_preflight.should_skip(), "empty source path should skip store");

    const std::string source = writeTempFile("/tmp/fuse_b79_would_inv.obj", "# would inv\n");
    desc.output_path = "/tmp/fuse_b79_would_inv.fusemesh";


    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source true for seeded entry");
               "would_invalidate_output true for seeded entry");
    expectTrue(!cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash),
               "would_invalidate_stale_content false when hash matches");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content true when hash mismatches");

    writeTempFile(source, "# would inv updated\n");
    expectTrue(cooker.cache().would_prune_all(), "would_prune_all true after on-disk source change");

    const fuse::u32 removed = cooker.cache().prune_stale_entries();
    expectTrue(removed == 1u, "prune removes entry probed by would_invalidate_stale_content");
    expectTrue(!cooker.cache().would_invalidate_source(source), "would_invalidate_source false after prune");

void testCookCacheEntryPreflightGuards() {
    const std::string source = writeTempFile("/tmp/fuse_b79_entry_preflight.obj", "# entry preflight\n");

    valid.content_hash = 909;
    valid.output_path = "/tmp/fuse_b79_entry_preflight.fusemesh";
    valid.kind = fuse::project::CookAssetKind::Mesh;
    expectTrue(fuse::project::preflight_cook_cache_entry(valid).ok(), "valid mesh cache entry passes preflight");

    fuse::project::CookCacheEntry zero_key = valid;
    zero_key.content_hash = 0;
    expectTrue(!fuse::project::preflight_cook_cache_entry(zero_key).ok(), "zero key fails entry preflight");
    expectTrue(fuse::project::preflight_cook_cache_entry(zero_key).reason ==
                   fuse::project::CookHashRejectReason::ZeroSourceHash,
               "zero key entry preflight reason");

    fuse::project::CookCacheEntry empty_source = valid;
    empty_source.source_path = "";
    expectTrue(fuse::project::preflight_cook_cache_entry(empty_source).reason ==
                   fuse::project::CookHashRejectReason::EmptyInputPath,
               "empty source entry preflight reason");

    fuse::project::CookCacheEntry shader_entry = valid;
    shader_entry.kind = fuse::project::CookAssetKind::Shader;
    shader_entry.output_path = "/tmp/fuse_b79_entry_preflight.fuseshader";
    expectTrue(!fuse::project::preflight_cook_cache_entry(shader_entry).ok(), "shader entry fails preflight");
    const std::vector<fuse::u64> probed = cooker.cache().probe_invalidation_hashes_for_source(source);
    expectTrue(probed.size() == 1u, "probe_invalidation_hashes finds one hash");
    expectTrue(probed[0] == seeded.content_hash, "probed hash matches seeded entry");
    expectTrue(!cooker.cache().would_prune_invalid(), "fresh entry would_prune_invalid is false");
    expectTrue(!cooker.cache().would_prune_stale(), "fresh entry would_prune_stale is false");

    const std::vector<fuse::u64> probed_source = cooker.cache().probe_invalidation_hashes_for_source(source);
    expectTrue(probed_source.size() == 1u, "probe_invalidation_hashes for source finds one hash");
    expectTrue(probed_source[0] == seeded.content_hash, "probed source hash matches seeded entry");

    const std::vector<fuse::u64> probed_output = cooker.cache().probe_invalidation_hashes_for_output(desc.output_path);
    expectTrue(probed_output.size() == 1u, "probe_invalidation_hashes for output finds one hash");
    expectTrue(probed_output[0] == seeded.content_hash, "probed output hash matches seeded entry");

    writeTempFile(source, "# would mesh updated\n");
    const fuse::u64 updated_hash = fuse::project::hash_mesh_import(desc);
    expectTrue(updated_hash != seeded.content_hash, "source change yields new content hash");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, updated_hash),
               "would_invalidate_stale_content true when current hash differs from stored entry");

    const fuse::project::CookCacheInvalidationSurface surface = cooker.cache().estimate_invalidation_surface();
    expectTrue(surface.entry_count == 1u, "invalidation surface reports one entry");
    expectTrue(surface.prunable_entries == 1u, "invalidation surface reports one prunable entry");
    expectTrue(surface.stale_entries == 1u, "invalidation surface reports one stale entry");
    expectTrue(surface.reconcile_total() == surface.prunable_entries,
               "invalidation surface reconcile total matches prunable count");
}

void testCookHashPreflightMtimeGuards() {
    const fuse::project::CookHashPreflight empty_mtime = fuse::project::preflight_file_mtime_ns("");
    expectTrue(!empty_mtime.ok(), "empty path fails mtime preflight");
    expectTrue(empty_mtime.reason == fuse::project::CookHashRejectReason::EmptyPath,
               "empty path mtime preflight reason is EmptyPath");

    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_mtime.obj", "# mtime preflight\n");
    expectTrue(fuse::project::preflight_file_mtime_ns(source).ok(), "readable path passes mtime preflight");
    expectTrue(fuse::project::file_mtime_ns(source) != 0, "mtime preflight success implies non-zero mtime");
    expectTrue(!cooker.cache().would_invalidate_source(""), "would_invalidate_source guarded on empty path");
    expectTrue(!cooker.cache().would_invalidate(0), "would_invalidate rejects zero hash");

void testCookHashShouldSkipGuards() {
    expectTrue(fuse::project::should_skip_file_content_hash(""), "should_skip rejects empty path");
    expectTrue(fuse::project::should_skip_combine_cook_cache_key(0, 42u),
               "should_skip rejects zero source cache key fold");

    const std::string source = writeTempFile("/tmp/fuse_b79_should_skip.obj", "# should skip\n");
    expectTrue(!fuse::project::should_skip_file_content_hash(source),
               "should_skip allows readable file path");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_should_skip.fusemesh";
    expectTrue(!fuse::project::should_skip_mesh_import_hash(desc),
               "should_skip allows valid mesh import preflight");
    expectTrue(fuse::project::should_skip_mesh_import_hash(desc) ==
                   fuse::project::preflight_mesh_import_hash(desc).should_skip(),
               "should_skip_mesh mirrors preflight should_skip");
    expectTrue(cooker.cache().would_prune_stale(), "stale entry would_prune_stale is true");
    expectTrue(cooker.cache().would_prune_all(), "stale entry would_prune_all agrees with would_prune_stale");
}

} // namespace

int main() {
    fuse::core::initialize();

    testCombineCookCacheKeyGuards();
    testCookCacheKeyPreflight();
    testCookHashPreflight();
    testCookCachePreflightLookupAndStore();
    testCookCacheCountProbes();
    testCookCacheStaleUpstreamCountProbe();
    testCookCacheEntryPreflight();
    testCookCachePruneEstimateProbes();
    testCookCacheStaleUpstreamProbes();
    testCookHashPreflightGuards();
    testCookCacheInvalidationProbes();
    testCookCacheReconcileEstimators();
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
    testCookHashPreflightFnvAndCacheEntryGuards();
    testCookHashPreflightGuards();
    testCookHashPreflightCacheEntryGuards();
    testCookHashTryPreflightAndShouldSkipGuards();
    testCookHashPreflightFnvAndCombineGuards();
    testCookHashPreflightShouldSkip();
    testCookCachePruneEstimateShouldSkip();
    testCookCacheWouldInvalidateProbes();
    testCookHashPreflightFnv1a64Guard();
    testCookCacheEntryPreflightGuards();
    testCookCacheReconcileEstimators();
    testCookCachePruneReconcileEstimate();
    testCookCachePreflightEntryGuards();
    testCookCacheWouldInvalidateSourceOutputGuards();
    testCookCacheWouldInvalidatePathProbes();
    testCookHashPreflightCacheableKeyGuards();
    testCookCacheUniqueStaleUpstreamProbe();
    testCookHashPreflightMtimeAndDependencyGuards();
    testCookCacheUniqueStaleUpstreamCount();
    testCookCacheWouldInvalidateGuards();
    testCookCacheUniqueStaleUpstreamProbes();
    testCookCacheInvalidationProbes();
    testCookCacheProbeUniqueStaleUpstreamSources();
    testCookHashPreflightManifestDependencyOutputs();
    testCookHashPreflightFileMtime();
    testCookCacheInvalidationEstimateGuards();
    testCookCachePreflightStoreEntry();
    testCookCachePruneReconcileEstimateGuards();
    testCookCacheProbeStaleContentSources();
    testCookCacheWouldInvalidationProbes();
    testContentHashValidityAndFnvGuards();
    testCookCacheStaleVsInvalidClassification();
    testCookCacheHasStaleInvalidAndCountGuards();
    testAssetCookerPruneStaleCache();
    testCookContentHashPreflightGuards();
    testCookCacheStaleUpstreamProbeParity();
    testCookCacheIncrementalInvalidationProbes();
    testCookCacheLookupStorePreflights();
    testCookFnvInputPreflight();
    testCookCachePruneEstimate();
    testCookCacheProbeAliases();
    testCookHashPreflightDeepenGuards();
    testCookCacheDeepenInvalidationProbes();
    testCookCacheWouldInvalidateMirrors();
    testCookCacheProbeStaleContentAndPruneEstimate();
    testCookHashPreflightFnvAndManifestCook();
    testCookCacheSourceOutputAndPruneEstimators();
    testCookCacheReconcileEstimate();
    testCookCacheStaleContentProbes();
    testCookHashPreflightCacheableAndManifestDeps();
    testCookCachePreflightCacheEntry();
    testCookCacheStaleUpstreamDedupProbe();
    testCookCacheWouldInvalidateSourceAndOutputProbes();
    testCookCachePreflightEntryAndManifestCoverage();
    testCookCacheStaleUpstreamUniqueProbe();
    testCookHashManifestDependencyPreflight();
    testCookCacheWouldInvalidateSourceOutputProbes();
    testCookCacheProbeStaleUpstreamSourcesUnique();
    testCookHashShaderAndManifestUpstreamPreflights();
    testCookHashPreflightImportPathsAndCacheEntry();
    testCookCacheWouldInvalidateShortcuts();
    testCookCacheProbeInvalidEntrySources();
    testCookHashPreflightCacheEntryAndManifestDeps();
    testCookHashPreflightManifestWithUpstream();
    testCookCacheEntryPreflight();
    testCookHashPreflightCacheEntryGuards();
    testCookHashManifestWithUpstreamPreflight();
    testCookCacheWouldInvalidateProbes();
    testCookCacheEntryPreflightGuards();
    testCookHashCacheEntryPreflight();
    testCookCacheProbeStaleUpstreamSourcePaths();
    testCookHashImportCacheKeyPreflight();
    testCookCachePruneInvalidEntriesOnLoad();
    testCookCacheLookupPreflightGuards();
    testCookCacheStorePreflightGuards();
    testCookImportHashPreflightGuards();
    testCookCacheInvalidationProbes();
    testCookCacheHashPreflightGuards();
    testCookCachePruneEstimateAndProbes();
    testCookHashPreflightGuards();
    testCookCacheReconcileEstimators();
    testContentHashPreflightGuards();
    testCookCachePreflightAndLookupGuards();
    testCookCacheReconcileEstimatorGuards();
    testCookCacheStaleClassificationGuards();
    testCookCacheReconcileEstimatorMatchesPrune();
    testCookCacheStaleUpstreamProbe();
    testCookCacheWouldInvalidateProbes();
    testCookCacheEntryPreflightGuards();
    testCookHashPreflightMtimeGuards();
    testCookHashShouldSkipGuards();
    testCookHashPreflightManifestCookKey();
    testCookHashPreflightImportCookKeys();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
