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

void testIspcTexcompHookMipLevelHeader() {
#if defined(FUSE_HAS_ISPC_TEXCOMP) && defined(FUSE_HAS_STB_IMAGE)
    static const unsigned char kMinimalPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
        0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0x0F, 0x00, 0x00,
        0x01, 0x01, 0x00, 0x05, 0x18, 0xD8, 0x4E, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
        0x42, 0x60, 0x82};
    {
        std::ofstream out("/tmp/fuse_ispc_mip.png", std::ios::binary);
        out.write(reinterpret_cast<const char*>(kMinimalPng), sizeof(kMinimalPng));
    }

    const fuse::cook::CookStubWriteResult written =
        fuse::cook::write_texture_stub("/tmp/fuse_ispc_mip.png", "/tmp/fuse_ispc_mip.fusetex", "BC7", true);
    expectTrue(written.ok, "texture cook with mipmaps ok");

    std::ifstream cooked("/tmp/fuse_ispc_mip.fusetex");
    std::string line;
    bool sawMipLevels = false;
    while (std::getline(cooked, line)) {
        if (line.find("mip_levels=") != std::string::npos) {
            sawMipLevels = true;
            break;
        }
    }
    expectTrue(sawMipLevels, "texture cook output records mip_levels");
#endif
}

void testIspcTexcompHookUnavailableWithoutInput() {
    const fuse::cook::CookStubWriteResult written =
        fuse::cook::tryCookTextureIspc("", "/tmp/fuse_ispc_output.fusetex", "BC7", false);
#if defined(FUSE_HAS_ISPC_TEXCOMP) && defined(FUSE_HAS_STB_IMAGE)
    expectTrue(!written.ok, "ispc hook rejects empty input");
#else
    expectTrue(!written.ok, "ispc hook unavailable without header");
#endif
}

void testGlslangShaderCookHook() {
    const std::string source = "/tmp/fuse_glslang_source.frag";
    {
        std::ofstream out(source, std::ios::trunc);
        out << "#version 450\n"
               "layout(location = 0) out vec4 outColor;\n"
               "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    }

    const fuse::cook::CookStubWriteResult written =
        fuse::cook::tryCookShaderGlslang(source, "/tmp/fuse_glslang_output.fuseshader", "fragment", 450u);

    std::ifstream cooked("/tmp/fuse_glslang_output.fuseshader");
    std::string header;
    if (cooked) {
        std::getline(cooked, header);
    }

    if (written.ok) {
        expectTrue(header == "FUSESHADER_GLSLANG", "glslang cook output marker");
        expectTrue(written.byteCount > 32u, "glslang cook output includes SPIR-V payload");
        return;
    }

    expectTrue(header.empty() || header != "FUSESHADER_GLSLANG",
               "glslang hook skipped cleanly when validator unavailable");
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
    testIspcTexcompHookMipLevelHeader();
    testIspcTexcompHookUnavailableWithoutInput();
    testGlslangShaderCookHook();
    testBc7CookWriter();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_bc7_encoder_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_bc7_encoder_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
