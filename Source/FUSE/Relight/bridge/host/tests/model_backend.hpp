// FUSE Relight RL-2.3 tests: a model D3D9 backend. Copyright (c) 2026 FUSE contributors (AGPL-3.0).
//
// Executes a subset of the real command schema against a tiny device model (render states, texture
// stages, one stream, textures with a 32-bit pixel grid, vertex buffers, state blocks) with D3D9's
// semantics where the journal's compaction depends on them:
// - Set* while a state block records are captured into the block, not applied;
// - Capture refreshes the recorded keys from the device, Apply applies them;
// - a Destroyed texture stays alive while a stage still binds it (the device holds a reference);
// - Unlock with D3DLOCK_DISCARD makes the whole buffer undefined (modelled as 0xDD) before writing;
// - UnlockRect's rect selects where its packed rows go (NULL = the whole level); uploads may come
//   through the shared heap (host side).
// GetRenderTargetData answers with a 64-bit digest of the whole model plus the draws of the current
// frame (Present resets the frame accumulator), so a client can compare a bridged/fallen-back
// device with a reference model fed the same commands directly.
#pragma once

#include <fuse/relight/bridge/host/link.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace rltest {

namespace host = fuse::relight::bridge::host;
namespace schema = fuse::relight::bridge::schema;
namespace cmd = fuse::relight::bridge::schema::cmd;

inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0x100000001b3ull;
}
inline uint64_t hashBytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

class ModelBackend final : public host::ILocalBackend {
public:
    struct Texture {
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> px;
    };
    struct Buffer {
        std::vector<uint8_t> bytes;
    };
    struct StateBlock {
        std::map<uint32_t, uint32_t> rs;
        std::map<uint32_t, std::shared_ptr<Texture>> tex;
        std::map<uint32_t, bool> texSet;
    };

    int32_t execute(uint16_t command, uint16_t, uint32_t handle, const uint8_t* data, size_t size,
                    host::Response* out) override {
        handle_ = handle;
        hr_ = host::kHrOk;
        bad_ = false;
        payload_.clear();
        ++executed;
        const schema::DecodeStatus st = schema::dispatch(command, data, size, [this](const auto& c) { this->on(c); });
        if (st != schema::DecodeStatus::Ok || bad_) {
            hr_ = host::kHrInvalidCall;
        }
        if (out != nullptr) {
            out->result = hr_;
            out->payload = payload_;
        }
        return hr_;
    }

    uint64_t digest() const {
        uint64_t h = mix(0, device_ ? 1 : 0);
        for (const auto& [k, v] : rs_) {
            h = mix(mix(h, k), v);
        }
        for (const auto& [s, t] : stages_) {
            h = mix(mix(h, s), texHash(t));
        }
        h = mix(h, stream_ ? hashBytes(stream_->bytes.data(), stream_->bytes.size()) : 0);
        for (const auto& [id, t] : textures_) {
            h = mix(mix(h, id), texHash(t));
        }
        for (const auto& [id, b] : buffers_) {
            h = mix(mix(h, id), hashBytes(b->bytes.data(), b->bytes.size()));
        }
        for (const auto& [id, sb] : blocks_) {
            h = mix(h, id);
            for (const auto& [k, v] : sb->rs) {
                h = mix(mix(h, k), v);
            }
            for (const auto& [s, t] : sb->tex) {
                h = mix(mix(h, s), texHash(t));
            }
        }
        return mix(h, frameDraws_);
    }

    void setSharedHeap(fuse::relight::bridge::ipc::SharedHeap* heap) override { heap_ = heap; }

    uint64_t executed = 0;
    uint64_t unknownHandles = 0;

private:
    static uint64_t texHash(const std::shared_ptr<Texture>& t) {
        return t ? hashBytes(t->px.data(), t->px.size() * 4, mix(t->w, t->h)) : 0;
    }
    template <class C>
    void on(const C&) {}  // everything else is a no-op in the model

    void on(const cmd::IDirect3D9Ex_CreateDevice& c) {
        device_ = c.result;
    }
    bool onDevice() {
        if (handle_ != device_ || device_ == 0) {
            ++unknownHandles;
            hr_ = host::kHrInvalidCall;
            return false;
        }
        return true;
    }
    void on(const cmd::IDirect3DDevice9Ex_SetRenderState& c) {
        if (!onDevice()) {
            return;
        }
        if (recording_) {
            recording_->rs[c.state] = c.value;
        } else {
            rs_[c.state] = c.value;
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetTexture& c) {
        if (!onDevice()) {
            return;
        }
        std::shared_ptr<Texture> t;
        if (c.texture != 0) {
            auto it = textures_.find(c.texture);
            if (it == textures_.end()) {
                ++unknownHandles;
                hr_ = host::kHrInvalidCall;
                return;
            }
            t = it->second;
        }
        if (recording_) {
            recording_->tex[c.stage] = t;
        } else if (t) {
            stages_[c.stage] = t;
        } else {
            stages_.erase(c.stage);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetStreamSource& c) {
        if (!onDevice()) {
            return;
        }
        auto it = buffers_.find(c.vertexBuffer);
        stream_ = it == buffers_.end() ? nullptr : it->second;
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateTexture& c) {
        if (!onDevice()) {
            return;
        }
        auto t = std::make_shared<Texture>();
        t->w = c.width;
        t->h = c.height;
        t->px.assign(static_cast<size_t>(c.width) * c.height, 0);
        textures_[c.result] = t;
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateVertexBuffer& c) {
        if (!onDevice()) {
            return;
        }
        auto b = std::make_shared<Buffer>();
        b->bytes.assign(c.length, 0);
        buffers_[c.result] = b;
    }
    void on(const cmd::IDirect3DResource9_Destroy&) {
        if (textures_.erase(handle_) + buffers_.erase(handle_) == 0) {
            ++unknownHandles;
        }
    }
    void on(const cmd::IDirect3DStateBlock9_Destroy&) {
        if (blocks_.erase(handle_) == 0) {
            ++unknownHandles;
        }
    }
    void on(const cmd::IDirect3DTexture9_UnlockRect& c) {
        auto it = textures_.find(handle_);
        if (it == textures_.end()) {
            ++unknownHandles;
            return;
        }
        host::UploadView v;
        if (!host::resolveUpload(heap_, c.heapChunk, c.heapBytes, c.data, v)) {
            bad_ = true;
            return;
        }
        Texture& t = *it->second;
        const bool whole = c.rect.size() != 4;
        const int32_t x0 = whole ? 0 : c.rect[0], y0 = whole ? 0 : c.rect[1];
        const int32_t x1 = whole ? static_cast<int32_t>(t.w) : c.rect[2], y1 = whole ? static_cast<int32_t>(t.h) : c.rect[3];
        for (int32_t y = y0; y < y1; ++y) {
            for (int32_t x = x0; x < x1; ++x) {
                const size_t src = static_cast<size_t>(y - y0) * c.rowBytes + static_cast<size_t>(x - x0) * 4;
                if (src + 4 <= v.size && x >= 0 && y >= 0 && static_cast<uint32_t>(x) < t.w && static_cast<uint32_t>(y) < t.h) {
                    std::memcpy(&t.px[static_cast<size_t>(y) * t.w + static_cast<size_t>(x)], v.data + src, 4);
                }
            }
        }
        host::releaseUpload(heap_, v);
    }
    void on(const cmd::IDirect3DVertexBuffer9_Unlock& c) {
        auto it = buffers_.find(handle_);
        if (it == buffers_.end()) {
            ++unknownHandles;
            return;
        }
        host::UploadView v;
        if (!host::resolveUpload(heap_, c.heapChunk, c.heapBytes, c.data, v)) {
            bad_ = true;
            return;
        }
        Buffer& b = *it->second;
        if (c.flags & 0x2000u) {  // D3DLOCK_DISCARD
            std::fill(b.bytes.begin(), b.bytes.end(), uint8_t(0xDD));
        }
        for (size_t i = 0; i < v.size && c.offset + i < b.bytes.size(); ++i) {
            b.bytes[c.offset + i] = v.data[i];
        }
        host::releaseUpload(heap_, v);
    }
    void on(const cmd::IDirect3DDevice9Ex_BeginStateBlock&) {
        if (onDevice()) {
            recording_ = std::make_shared<StateBlock>();
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_EndStateBlock& c) {
        if (onDevice() && recording_) {
            blocks_[c.result] = recording_;
            recording_.reset();
        }
    }
    void on(const cmd::IDirect3DStateBlock9_Capture&) {
        auto it = blocks_.find(handle_);
        if (it == blocks_.end()) {
            ++unknownHandles;
            return;
        }
        for (auto& [k, v] : it->second->rs) {
            auto cur = rs_.find(k);
            v = cur == rs_.end() ? 0 : cur->second;
        }
        for (auto& [s, t] : it->second->tex) {
            auto cur = stages_.find(s);
            t = cur == stages_.end() ? nullptr : cur->second;
        }
    }
    void on(const cmd::IDirect3DStateBlock9_Apply&) {
        auto it = blocks_.find(handle_);
        if (it == blocks_.end()) {
            ++unknownHandles;
            return;
        }
        for (const auto& [k, v] : it->second->rs) {
            rs_[k] = v;
        }
        for (const auto& [s, t] : it->second->tex) {
            if (t) {
                stages_[s] = t;
            } else {
                stages_.erase(s);
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_Clear& c) {
        if (onDevice()) {
            frameDraws_ = mix(frameDraws_, mix(0xC1EA, c.color));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DrawPrimitive& c) {
        if (onDevice()) {
            uint64_t s = mix(0xD7A3, c.primitiveCount);
            for (const auto& [k, v] : rs_) {
                s = mix(mix(s, k), v);
            }
            for (const auto& [st, t] : stages_) {
                s = mix(mix(s, st), texHash(t));
            }
            s = mix(s, stream_ ? hashBytes(stream_->bytes.data(), stream_->bytes.size()) : 0);
            frameDraws_ = mix(frameDraws_, s);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_Present&) {
        if (onDevice()) {
            frameDraws_ = 0;
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_GetRenderTargetData&) {
        if (onDevice()) {
            const uint64_t d = digest();
            payload_.resize(8);
            std::memcpy(payload_.data(), &d, 8);
        }
    }

    fuse::relight::bridge::ipc::SharedHeap* heap_ = nullptr;
    bool bad_ = false;
    uint32_t handle_ = 0;
    int32_t hr_ = 0;
    std::vector<uint8_t> payload_;
    uint32_t device_ = 0;
    std::map<uint32_t, uint32_t> rs_;
    std::map<uint32_t, std::shared_ptr<Texture>> stages_;
    std::shared_ptr<Buffer> stream_;
    std::map<uint32_t, std::shared_ptr<Texture>> textures_;
    std::map<uint32_t, std::shared_ptr<Buffer>> buffers_;
    std::map<uint32_t, std::shared_ptr<StateBlock>> blocks_;
    std::shared_ptr<StateBlock> recording_;
    uint64_t frameDraws_ = 0;
};

}  // namespace rltest
