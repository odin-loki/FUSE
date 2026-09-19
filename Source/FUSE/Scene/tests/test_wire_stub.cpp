#include <fuse/scene/wire_stub.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testWireStubParse() {
    expectTrue(!fuse::scene::isWireStubEntityName("GroundPlane"), "regular entity is not wire stub");
    expectTrue(fuse::scene::isWireStubEntityName("__fuse.wire|material|GroundPlane|Prototyping:FloorGray"),
               "wire stub prefix detected");

    const fuse::scene::WireStubRef wire =
        fuse::scene::parseWireStubEntityName("__fuse.wire|datablock|SpawnSphere|SpawnSphereMarker");
    expectTrue(wire.valid, "wire stub parsed");
    expectTrue(wire.kind == "datablock", "wire kind parsed");
    expectTrue(wire.owner == "SpawnSphere", "wire owner parsed");
    expectTrue(wire.value == "SpawnSphereMarker", "wire value parsed");
}

} // namespace

int main() {
    testWireStubParse();

    if (g_failures == 0) {
        std::printf("fuse_scene_wire_stub_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_scene_wire_stub_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
