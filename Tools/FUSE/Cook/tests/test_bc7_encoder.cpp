#include <fuse/cook/bc7_encoder.hpp>
#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/core/init.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testBc7SolidBlockRoundTrip() {
    fuse::u8 block[16];
    fuse::cook::bc7_encode_solid_block(block, 128, 64, 32, 255);

    fuse::u8 r = 0;
    fuse::u8 g = 0;
    fuse::u8 b = 0;
    fuse::u8 a = 0;
    expectTrue(fuse::cook::bc7_decode_solid_block(block, r, g, b, a), "bc7 decode ok");
    expectTrue(r >= 120 && r <= 136, "bc7 red round-trip within tolerance");
    expectTrue(g >= 56 && g <= 72, "bc7 green round-trip within tolerance");
    expectTrue(b >= 24 && b <= 40, "bc7 blue round-trip within tolerance");
}

void testBc7DualEndpointBlock() {
    fuse::u8 rgba[64];
    for (fuse::u32 i = 0; i < 16u; ++i) {
        const fuse::u8 value = (i < 8u) ? 32u : 224u;
        rgba[i * 4u + 0] = value;
        rgba[i * 4u + 1] = value;
        rgba[i * 4u + 2] = value;
        rgba[i * 4u + 3] = 255u;
    }

    fuse::u8 block[16];
    fuse::cook::bc7_encode_dual_endpoint_block(rgba, block);

    fuse::u8 decoded[64];
    expectTrue(fuse::cook::bc7_decode_dual_endpoint_block(block, decoded), "dual endpoint decode ok");
    expectTrue(decoded[0] < 96u, "dark endpoint preserved");
    expectTrue(decoded[60] > 160u, "bright endpoint preserved");
}

void testBc7EncodeImageBlocks() {
    const fuse::u8 rgba[] = {
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
    };

    std::vector<fuse::u8> blocks;
    const fuse::cook::Bc7EncodeResult encoded =
        fuse::cook::encode_bc7_rgba8(rgba, 4u, 4u, blocks);
    expectTrue(encoded.ok, "bc7 image encode ok");
    expectTrue(encoded.blockCount == 1u, "4x4 image is one block");
    expectTrue(blocks.size() == 16u, "one bc7 block is 16 bytes");
}

void testBc7CookWriter() {
    const std::string source = "/tmp/fuse_bc7_source.bin";
    {
        std::ofstream out(source, std::ios::binary);
        out << "texture-bytes";
    }

    const fuse::cook::CookStubWriteResult written =
        fuse::cook::write_texture_bc7_encoded("/tmp/fuse_bc7_output.fusetex", source, true);
    expectTrue(written.ok, "bc7 cook writer ok");
    expectTrue(written.byteCount > 32u, "bc7 output includes header and block payload");

    std::ifstream cooked("/tmp/fuse_bc7_output.fusetex", std::ios::binary);
    std::string header;
    cooked >> header;
    expectTrue(header == "FUSETEX_BC7", "bc7 cook output marker");
}

} // namespace

int main() {
    fuse::core::initialize();
    testBc7SolidBlockRoundTrip();
    testBc7DualEndpointBlock();
    testBc7EncodeImageBlocks();
    testBc7CookWriter();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_bc7_encoder_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_bc7_encoder_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
