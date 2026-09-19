#include <fuse/cook/cook_stub_writer.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fuse::cook {

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

} // namespace

CookStubWriteResult write_mesh_stub(const std::string& output_path, u32 lod_count, bool compressed) {
    std::ostringstream payload;
    payload << "FUSEMESH_STUB\n";
    payload << "lods=" << lod_count << "\n";
    payload << "compress=" << (compressed ? "on" : "off") << "\n";
    payload << "vertices=0\n";
    payload << "indices=0\n";
    return writeTextStub(output_path, payload.str());
}

CookStubWriteResult write_texture_stub(const std::string& output_path, const char* compression,
                                       bool mipmaps) {
    std::ostringstream payload;
    payload << "FUSETEX_STUB\n";
    payload << "compression=" << compression << "\n";
    payload << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    payload << "width=0\n";
    payload << "height=0\n";
    return writeTextStub(output_path, payload.str());
}

CookStubWriteResult write_audio_stub(const std::string& output_path, u32 sample_rate,
                                     const char* format) {
    std::ostringstream payload;
    payload << "FUSEAUDIO_STUB\n";
    payload << "rate=" << sample_rate << "\n";
    payload << "format=" << format << "\n";
    payload << "samples=0\n";
    return writeTextStub(output_path, payload.str());
}

} // namespace fuse::cook
