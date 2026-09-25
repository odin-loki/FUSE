// FUSE Relight RL-1.1: the recording (mock) tap. Writes every event as one JSON object per line
// (JSON Lines, schema "fuse.relight.tap_events/1") so tests can compare the event stream with
// what an app fed D3D9 (Source/FUSE/Relight/tests/tap/rl_tap_expect.py derives the expected
// stream from an RL-0.4 app sidecar). relight.tap.mode = record.
//
// Line types ("ev"), in call order:
//   header          schema, interface version
//   device_create / device_reset / device_destroy
//   texture_create, texture_upload (sha256 of the locked bytes), texture_copy (UpdateSurface: the
//                   copied width / height and dest_x / dest_y), texture_write_lock,
//   image_destroy
//   buffer_create, buffer_write (sha256 of the tap's shadow of the whole buffer after the write:
//                   bytes the application wrote; D3DLOCK_DISCARD zeroes the shadow first, as the
//                   RL-0.4 sidecars model it), buffer_destroy
//   state_block     a deduplicated draw state (render / stage / sampler states by name, textures,
//                   lights, material, viewport, targets, the six clip planes, the render-target
//                   alpha-swizzle mask or null, non-zero shader constants), written once before the
//                   first draw that references it
//   shader          (RL-1.6) a shader's id, version, bytecode sha256 and bytecode (hex), written once
//                   before the first draw that binds it
//   draw            call arguments, vertex elements, stream / index bindings with the shadow
//                   sha256 of each bound buffer, UP data sha256, non-identity transforms,
//                   shaders (id, version, bytecode sha256), state block index, the light and
//                   clip-plane change counters (lights_version, clip_planes_version), decision
//   clear, set_render_target, query_begin, query_end, inject_point, present
// Every line carries "frame" (presents so far). Content is hashed, never dumped, except shader
// bytecode (the replay tools hash vertex shaders, and a D3D8 app's shaders reach the tap translated).
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::relight::tap {

class RecordingTap final : public IRelightTap {
public:
    explicit RecordingTap(std::string path);
    ~RecordingTap() override;
    RecordingTap(const RecordingTap&) = delete;
    RecordingTap& operator=(const RecordingTap&) = delete;

    bool isOpen() const { return m_file != nullptr; }
    const std::string& path() const { return m_path; }

    void onDeviceCreate(const DeviceEvent& e) override;
    void onDeviceReset(const DeviceEvent& e) override;
    void onDeviceDestroy() override;
    void onTextureCreate(const TextureDesc& d) override;
    void onTextureUpload(const TextureUpload& u) override;
    void onTextureCopy(const TextureCopy& c) override;
    void onTextureWriteLock(const TextureWriteLock& l) override;
    void onImageDestroy(const ImageDestroy& d) override;
    void onBufferCreate(const BufferDesc& d) override;
    void onBufferWrite(const BufferWrite& w) override;
    void onBufferDestroy(ResourceId id) override;
    DrawDecision onDraw(const DrawCall& call, const DrawState& state) override;
    void onQueryBegin(const QueryEvent& q) override;
    void onQueryEnd(const QueryEvent& q) override;
    void onClear(const ClearEvent& c) override;
    void onSetRenderTarget(const SetRenderTargetEvent& e) override;
    void onInjectPoint(const FrameEvent& f) override;
    void onPresent(const FrameEvent& f) override;

private:
    void writeLine(const std::string& line);
    void deviceLine(const char* ev, const DeviceEvent& e);
    std::string shaderJson(const ShaderRef& s);
    std::string bufferSha(ResourceId id);
    std::size_t stateBlock(const DrawState& s);

    std::mutex m_mutex;
    std::string m_path;
    std::FILE* m_file = nullptr;
    std::uint64_t m_frame = 0;
    bool m_destroyed = false;
    std::map<ResourceId, std::vector<std::uint8_t>> m_bufferShadow;
    std::map<ResourceId, std::string> m_bufferShaCache; ///< invalidated on write
    std::map<ResourceId, std::string> m_shaderSha;
    std::map<std::string, std::size_t> m_stateBlocks;
};

} // namespace fuse::relight::tap
