#include <fuse/cook/cook_stub_writer.hpp>

#include <fuse/cook/bc7_encoder.hpp>
#include <fuse/cook/ispc_texcomp_hook.hpp>
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/cook/texture_cook.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <cstring>

#if defined(FUSE_HAS_OGG_VORBIS)
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#endif

namespace fuse::cook {

namespace {

struct WavHeaderInfo {
    bool valid = false;
    u32 channels = 0;
    u32 sampleRate = 0;
    u16 bitsPerSample = 16;
    u32 dataBytes = 0;
    u32 dataOffset = 0;
};

WavHeaderInfo sniffWavHeader(std::ifstream& in) {
    WavHeaderInfo info;
    const std::streampos start = in.tellg();

    char riff[4] = {};
    in.read(riff, 4);
    if (riff[0] != 'R' || riff[1] != 'I' || riff[2] != 'F' || riff[3] != 'F') {
        return info;
    }

    in.ignore(4);
    char wave[4] = {};
    in.read(wave, 4);
    if (wave[0] != 'W' || wave[1] != 'A' || wave[2] != 'V' || wave[3] != 'E') {
        return info;
    }

    u16 audioFormat = 0;
    u16 channels = 0;
    u32 sampleRate = 0;
    u16 bitsPerSample = 16;
    bool fmtFound = false;

    while (in && !in.eof()) {
        char chunkId[4] = {};
        in.read(chunkId, 4);
        if (!in) {
            break;
        }

        u32 chunkSize = 0;
        in.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (!in) {
            break;
        }

        if (chunkId[0] == 'f' && chunkId[1] == 'm' && chunkId[2] == 't' && chunkId[3] == ' ') {
            in.read(reinterpret_cast<char*>(&audioFormat), 2);
            in.read(reinterpret_cast<char*>(&channels), 2);
            in.read(reinterpret_cast<char*>(&sampleRate), 4);
            in.ignore(4);
            in.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (chunkSize > 16u) {
                in.ignore(static_cast<std::streamoff>(chunkSize - 16u));
            }
            fmtFound = true;
        } else if (chunkId[0] == 'd' && chunkId[1] == 'a' && chunkId[2] == 't' && chunkId[3] == 'a') {
            info.dataOffset = static_cast<u32>(in.tellg() - start);
            info.dataBytes = chunkSize;
            break;
        } else {
            in.ignore(static_cast<std::streamoff>(chunkSize));
        }
    }

    if (!fmtFound || audioFormat != 1u || channels == 0u || sampleRate == 0u || info.dataBytes == 0u) {
        return info;
    }

    info.valid = true;
    info.channels = channels;
    info.sampleRate = sampleRate;
    info.bitsPerSample = bitsPerSample > 0u ? bitsPerSample : 16u;
    return info;
}

bool readWavPcm16(std::ifstream& in, const WavHeaderInfo& wav, std::vector<std::int16_t>& pcm) {
    if (!wav.valid || wav.dataOffset == 0u || wav.dataBytes == 0u || wav.bitsPerSample != 16u) {
        return false;
    }

    in.seekg(static_cast<std::streamoff>(wav.dataOffset));
    const u32 sampleCount = wav.dataBytes / (wav.channels * 2u);
    pcm.resize(sampleCount * wav.channels);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(wav.dataBytes));
    return in.good();
}

} // namespace

namespace {

CookStubWriteResult writeTextStub(const std::string& output_path, const std::string& payload) {
    CookStubWriteResult result;
    result.byteCount = static_cast<u32>(payload.size());

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "unable to write stub output";
        return result;
    }

    out << payload;
    result.ok = out.good();
    result.note = result.ok ? "stub output written" : "stub output write failed";
    return result;
}

CookStubWriteResult unavailableHook(const char* hookName) {
    CookStubWriteResult result;
    result.note = std::string(hookName) + " unavailable (library not linked)";
    return result;
}

} // namespace

CookStubWriteResult tryCookMeshAssimp(const std::string& input_path, const std::string& output_path,
                                      u32 lod_count, bool compressed) {
    // LOD generation and vertex compression are not implemented yet; the knobs only feed the
    // cache key. The cooked output is the lossless FMSH binary (see mesh_cook.hpp).
    (void)lod_count;
    (void)compressed;
    return cook_mesh_file(input_path, output_path);
}

CookStubWriteResult tryCookTextureBc7(const std::string& input_path, const std::string& output_path,
                                      const char* compression, bool mipmaps) {
    (void)compression; // BC5 / BC1 paths are not implemented; every texture cooks to BC7 mode 6.
    return cook_texture_bc7_file(input_path, output_path, mipmaps);
}

CookStubWriteResult tryCookAudioOgg(const std::string& input_path, const std::string& output_path,
                                    u32 sample_rate, const char* format) {
#if defined(FUSE_HAS_OGG_VORBIS)
    if (input_path.empty() || output_path.empty()) {
        CookStubWriteResult result;
        result.note = "ogg missing input or output path";
        return result;
    }

    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        CookStubWriteResult result;
        result.note = "ogg source unreadable";
        return result;
    }

    const WavHeaderInfo wav = sniffWavHeader(in);
    const u32 effectiveRate = wav.valid ? wav.sampleRate : sample_rate;
    const u32 effectiveChannels = wav.valid ? wav.channels : 2u;

    std::vector<std::int16_t> pcm;
    u32 pcmSamples = 0;
    if (wav.valid) {
        in.seekg(0);
        if (readWavPcm16(in, wav, pcm)) {
            pcmSamples = wav.channels > 0u ? static_cast<u32>(pcm.size() / wav.channels) : 0u;
        }
    }

    vorbis_info vorbisInfo;
    vorbis_info_init(&vorbisInfo);
    const int vorbisSetup = vorbis_encode_init(&vorbisInfo, static_cast<long>(effectiveChannels),
                                               static_cast<long>(effectiveRate), 128000, 160000, 192000);
    const bool vorbisReady = vorbisSetup == 0;

    std::string encoderNote = vorbisReady ? "vorbisenc_init_ok" : "vorbisenc_stub";
    std::vector<u8> oggPages;

    if (vorbisReady && !pcm.empty() && pcmSamples > 0u) {
        vorbis_comment vorbisComment;
        vorbis_comment_init(&vorbisComment);
        vorbis_dsp_state vorbisDsp;
        vorbis_block vorbisBlock;

        vorbis_analysis_init(&vorbisDsp, &vorbisInfo);
        vorbis_block_init(&vorbisDsp, &vorbisBlock);

        ogg_packet headerPacket{};
        ogg_packet headerCommentPacket{};
        ogg_packet headerCodePacket{};
        vorbis_analysis_headerout(&vorbisDsp, &vorbisComment, &headerPacket, &headerCommentPacket,
                                  &headerCodePacket);

        ogg_stream_state oggStream;
        ogg_stream_init(&oggStream, 0xF010001u);

        auto flushOggPage = [&](ogg_page& page) {
            oggPages.insert(oggPages.end(), page.header, page.header + page.header_len);
            oggPages.insert(oggPages.end(), page.body, page.body + page.body_len);
        };

        auto writeOggPacket = [&](ogg_packet& packet) {
            ogg_stream_packetin(&oggStream, &packet);
            ogg_page page;
            while (ogg_stream_pageout(&oggStream, &page) != 0) {
                flushOggPage(page);
            }
        };

        writeOggPacket(headerPacket);
        writeOggPacket(headerCommentPacket);
        writeOggPacket(headerCodePacket);

        const u32 blockSize = 1024u;
        for (u32 offset = 0; offset < pcmSamples; offset += blockSize) {
            const u32 frameCount = std::min(blockSize, pcmSamples - offset);
            float** analysisBuffer = vorbis_analysis_buffer(&vorbisDsp, static_cast<int>(frameCount));
            for (u32 frame = 0; frame < frameCount; ++frame) {
                for (u32 channel = 0; channel < effectiveChannels; ++channel) {
                    const std::int16_t sample = pcm[(offset + frame) * effectiveChannels + channel];
                    analysisBuffer[channel][frame] = static_cast<float>(sample) / 32768.f;
                }
            }
            vorbis_analysis_wrote(&vorbisDsp, static_cast<int>(frameCount));
            while (vorbis_analysis_blockout(&vorbisDsp, &vorbisBlock) == 1) {
                vorbis_analysis(&vorbisBlock, nullptr);
                vorbis_bitrate_addblock(&vorbisBlock);
                ogg_packet dataPacket;
                while (vorbis_bitrate_flushpacket(&vorbisDsp, &dataPacket) != 0) {
                    writeOggPacket(dataPacket);
                }
            }
        }

        vorbis_analysis_wrote(&vorbisDsp, 0);
        while (vorbis_analysis_blockout(&vorbisDsp, &vorbisBlock) == 1) {
            vorbis_analysis(&vorbisBlock, nullptr);
            vorbis_bitrate_addblock(&vorbisBlock);
            ogg_packet dataPacket;
            while (vorbis_bitrate_flushpacket(&vorbisDsp, &dataPacket) != 0) {
                writeOggPacket(dataPacket);
            }
        }

        ogg_page page;
        while (ogg_stream_flush(&oggStream, &page) != 0) {
            flushOggPage(page);
        }

        ogg_stream_clear(&oggStream);
        vorbis_block_clear(&vorbisBlock);
        vorbis_dsp_clear(&vorbisDsp);
        vorbis_comment_clear(&vorbisComment);
        encoderNote = oggPages.size() > 64u ? "vorbisenc_encode_ok" : "vorbisenc_encode_empty";
    }

    vorbis_info_clear(&vorbisInfo);

    if (!oggPages.empty()) {
        std::ostringstream header;
        header << "FUSEAUDIO_OGG\n";
        header << "hook=ogg\n";
        header << "rate=" << effectiveRate << "\n";
        header << "format=" << format << "\n";
        header << "channels=" << effectiveChannels << "\n";
        header << "samples=" << pcmSamples << "\n";
        header << "encoder=" << encoderNote << "\n";
        header << "wav=" << (wav.valid ? "yes" : "no") << "\n";
        header << "DATA\n";

        std::string payload = header.str();
        payload.append(reinterpret_cast<const char*>(oggPages.data()), oggPages.size());

        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
        CookStubWriteResult written;
        if (!out) {
            written.note = "unable to write ogg output";
            return written;
        }
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        written.ok = out.good();
        written.byteCount = static_cast<u32>(payload.size());
        written.note = written.ok ? ("ogg encoded, pages=" + std::to_string(oggPages.size())) : "ogg write failed";
        return written;
    }

    std::ostringstream payload;
    payload << "FUSEAUDIO_STUB\n";
    payload << "hook=ogg\n";
    payload << "rate=" << effectiveRate << "\n";
    payload << "format=" << format << "\n";
    payload << "channels=" << effectiveChannels << "\n";
    payload << "samples=" << pcmSamples << "\n";
    payload << "encoder=" << encoderNote << "\n";
    payload << "wav=" << (wav.valid ? "yes" : "no") << "\n";

    CookStubWriteResult written = writeTextStub(output_path, payload.str());
    if (written.ok) {
        written.note = "ogg encoder hook wrote stub container";
    }
    return written;
#else
    (void)input_path;
    (void)output_path;
    (void)sample_rate;
    (void)format;
    return unavailableHook("ogg");
#endif
}

CookStubWriteResult write_mesh_stub(const std::string& input_path, const std::string& output_path,
                                    u32 lod_count, bool compressed) {
    CookStubWriteResult hook{};
    if (!input_path.empty()) {
        hook = tryCookMeshAssimp(input_path, output_path, lod_count, compressed);
        if (hook.ok) {
            return hook;
        }
    } else {
        hook.note = "missing_input_path";
    }

    std::ostringstream payload;
    payload << "FUSEMESH_STUB\n";
    payload << "hook=" << hook.note << "\n";
    payload << "lods=" << lod_count << "\n";
    payload << "compress=" << (compressed ? "on" : "off") << "\n";
    payload << "vertices=0\n";
    payload << "indices=0\n";
    return writeTextStub(output_path, payload.str());
}

CookStubWriteResult write_texture_bc7_encoded(const std::string& output_path,
                                              const std::string& source_path, bool mipmaps) {
    if (!source_path.empty()) {
        const CookStubWriteResult decoded = cook_texture_bc7_file(source_path, output_path, mipmaps);
        if (decoded.ok) {
            return decoded;
        }
    }

    // Lenient fallback for undecodable sources: a deterministic placeholder image, labelled as such
    // in the header. Strict cooks (AssetCooker::set_strict_import) never reach this path.
    Bc7RgbaImage working = source_path.empty() ? Bc7RgbaImage{} : synthesize_rgba_from_source(source_path);
    if (working.rgba.empty()) {
        working.width = 4u;
        working.height = 4u;
        working.rgba.clear();
        for (u32 i = 0; i < 16u; ++i) {
            working.rgba.insert(working.rgba.end(), {200, 64, 32, 255});
        }
    }
    return write_texture_bc7_rgba(working.rgba.data(), working.width, working.height, output_path, mipmaps,
                                  "bc7_synthesized_placeholder");
}

CookStubWriteResult write_texture_stub(const std::string& input_path, const std::string& output_path,
                                       const char* compression, bool mipmaps) {
    const CookStubWriteResult ispc = tryCookTextureIspc(input_path, output_path, compression, mipmaps);
    if (ispc.ok) {
        return ispc;
    }

    if (compression != nullptr && std::string(compression) == "BC7") {
        const CookStubWriteResult bc7 = tryCookTextureBc7(input_path, output_path, compression, mipmaps);
        if (bc7.ok) {
            return bc7;
        }
        const CookStubWriteResult fallback = write_texture_bc7_encoded(output_path, input_path, mipmaps);
        if (fallback.ok) {
            return fallback;
        }
    }

    CookStubWriteResult hook{};
    if (!input_path.empty()) {
        hook = tryCookTextureBc7(input_path, output_path, compression, mipmaps);
        if (hook.ok) {
            return hook;
        }
    } else {
        hook.note = "missing_input_path";
    }

    std::ostringstream payload;
    payload << "FUSETEX_STUB\n";
    payload << "hook=" << hook.note << "\n";
    payload << "compression=" << compression << "\n";
    payload << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    payload << "width=0\n";
    payload << "height=0\n";
    return writeTextStub(output_path, payload.str());
}

CookStubWriteResult write_audio_stub(const std::string& input_path, const std::string& output_path,
                                     u32 sample_rate, const char* format) {
    const CookStubWriteResult hook = tryCookAudioOgg(input_path, output_path, sample_rate, format);
    if (hook.ok) {
        return hook;
    }

    std::ostringstream payload;
    payload << "FUSEAUDIO_STUB\n";
    payload << "hook=" << hook.note << "\n";
    payload << "rate=" << sample_rate << "\n";
    payload << "format=" << format << "\n";
    payload << "samples=0\n";
    return writeTextStub(output_path, payload.str());
}

CookStubWriteResult tryCookShaderGlslang(const std::string& input_path, const std::string& output_path,
                                         const char* stage, u32 target_version) {
    CookStubWriteResult result;
    if (input_path.empty() || output_path.empty()) {
        result.note = "missing_input_or_output_path";
        return result;
    }

    const std::filesystem::path input = std::filesystem::path(input_path);
    const std::string ext = input.extension().string();
    if (ext != ".cs" && ext != ".glsl" && ext != ".frag" && ext != ".vert" && ext != ".comp") {
        result.note = "glslang_source_required";
        return result;
    }

    const char* stageFlag = "frag";
    if (stage != nullptr) {
        if (std::strcmp(stage, "vertex") == 0) {
            stageFlag = "vert";
        } else if (std::strcmp(stage, "compute") == 0 || ext == ".cs" || ext == ".comp") {
            stageFlag = "comp";
        }
    } else if (ext == ".vert") {
        stageFlag = "vert";
    } else if (ext == ".cs" || ext == ".comp") {
        stageFlag = "comp";
    }

    const std::filesystem::path tempSpv =
        std::filesystem::temp_directory_path() / "fuse_glslang_spv.bin";
    const std::string tempSpvPath = tempSpv.string();

    std::ostringstream command;
    command << "glslangValidator -V -S " << stageFlag << " \"" << input_path << "\" -o \""
            << tempSpvPath << "\"";
    const int compileResult = std::system(command.str().c_str());
    if (compileResult != 0) {
        result.note = "glslang_unavailable_or_failed";
        std::error_code ec;
        std::filesystem::remove(tempSpv, ec);
        return result;
    }

    std::ifstream in(tempSpvPath, std::ios::binary);
    if (!in) {
        result.note = "glslang_output_unreadable";
        std::error_code ec;
        std::filesystem::remove(tempSpv, ec);
        return result;
    }

    std::vector<char> spirv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::error_code ec;
    std::filesystem::remove(tempSpv, ec);
    if (spirv.empty() || (spirv.size() % 4u) != 0u) {
        result.note = "glslang_invalid_spirv_size";
        return result;
    }

    std::ostringstream header;
    header << "FUSESHADER_GLSLANG\n";
    header << "stage=" << (stage != nullptr ? stage : "fragment") << "\n";
    header << "version=" << target_version << "\n";
    header << "words=" << (spirv.size() / 4u) << "\n";
    header << "DATA\n";

    std::string payload = header.str();
    payload.append(spirv.data(), spirv.size());

    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "glslang_output_unwritable";
        return result;
    }

    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    result.ok = out.good();
    result.byteCount = static_cast<u32>(payload.size());
    result.note = result.ok ? "glslang offline compile written" : "glslang write failed";
    return result;
}

CookStubWriteResult tryCookShaderSpirv(const std::string& input_path, const std::string& output_path,
                                       const char* stage, u32 target_version) {
    CookStubWriteResult result;
    if (input_path.empty() || output_path.empty()) {
        result.note = "missing_input_or_output_path";
        return result;
    }

    const std::filesystem::path input = std::filesystem::path(input_path);
    const std::string ext = input.extension().string();
    if (ext != ".spv") {
        result.note = "spirv_input_required";
        return result;
    }

    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        result.note = "spirv_input_unreadable";
        return result;
    }

    std::vector<char> spirv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (spirv.empty() || (spirv.size() % 4u) != 0u) {
        result.note = "spirv_invalid_size";
        return result;
    }

    std::ostringstream header;
    header << "FUSESHADER_SPIV\n";
    header << "stage=" << (stage != nullptr ? stage : "fragment") << "\n";
    header << "version=" << target_version << "\n";
    header << "words=" << (spirv.size() / 4u) << "\n";
    header << "DATA\n";

    std::string payload = header.str();
    payload.append(spirv.data(), spirv.size());

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "spirv_output_unwritable";
        return result;
    }

    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    result.ok = out.good();
    result.byteCount = static_cast<u32>(payload.size());
    result.note = result.ok ? "spirv passthrough written" : "spirv write failed";
    return result;
}

CookStubWriteResult write_shader_stub(const std::string& input_path, const std::string& output_path,
                                      const char* stage, u32 target_version) {
    const CookStubWriteResult glslangHook =
        tryCookShaderGlslang(input_path, output_path, stage, target_version);
    if (glslangHook.ok) {
        return glslangHook;
    }

    const CookStubWriteResult hook =
        tryCookShaderSpirv(input_path, output_path, stage, target_version);
    if (hook.ok) {
        return hook;
    }

    std::ostringstream payload;
    payload << "FUSESHADER_STUB\n";
    payload << "hook=" << hook.note << "\n";
    payload << "stage=" << (stage != nullptr ? stage : "fragment") << "\n";
    payload << "version=" << target_version << "\n";
    payload << "spirv_words=0\n";
    return writeTextStub(output_path, payload.str());
}

} // namespace fuse::cook
