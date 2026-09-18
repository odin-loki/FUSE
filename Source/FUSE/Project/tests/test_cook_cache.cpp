#include <fuse/core/init.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_content_hash.hpp>

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

    expectTrue(cache.prune_stale_entries() == 0u, "prune_stale on empty cache returns zero");
    expectTrue(cache.prune_invalid_entries() == 0u, "prune_invalid on empty cache returns zero");
    expectTrue(cache.prune_all() == 0u, "prune_all on empty cache returns zero");

    expectTrue(cache.invalidate_source("") == 0u, "empty source path invalidation is a no-op");
    expectTrue(cache.invalidate_output("") == 0u, "empty output path invalidation is a no-op");
    expectTrue(cache.invalidate_stale_content_for_source("", 42u) == 0u,
               "stale-content invalidation rejects empty source path");
    expectTrue(cache.invalidate_stale_content_for_source("/tmp/fuse_b79_missing.obj", 0u) == 0u,
               "stale-content invalidation rejects zero content hash");

    cache.invalidate_all();
    expectTrue(cache.stats().invalidations == 0u, "invalidate_all on empty cache does not bump stats");

    expectTrue(!cache.save(""), "save rejects empty path");
    expectTrue(!cache.load(""), "load rejects empty path");
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

void testCookCachePruneInvalidEntriesOnLoad() {
    const std::string cachePath = "/tmp/fuse_b79_prune_invalid_load.json";
    {
        std::ofstream out(cachePath, std::ios::binary);
        out << R"({
  "schemaVersion": 1,
  "entries": [
    {
      "contentHash": 201,
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
    expectTrue(loaded.contains(201u), "valid loaded entry remains addressable");
}

} // namespace

int main() {
    fuse::core::initialize();

    testCombineCookCacheKeyGuards();
    testCookCacheEmptyPathRejection();
    testCookCacheEmptyGuards();
    testCookCacheZeroKeyGuards();
    testCookCachePruneAll();
    testCookCacheStaleContentInvalidationGuards();
    testCookCachePruneInvalidEntriesOnLoad();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
