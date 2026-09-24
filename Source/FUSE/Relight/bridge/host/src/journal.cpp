// FUSE Relight RL-2.3: command journal for the crash fallback (see journal.hpp).
// Copyright (c) 2026 FUSE contributors (MIT). New code.

#include <fuse/relight/bridge/host/journal.hpp>
#include <fuse/relight/bridge/host/protocol.hpp>

#include <array>
#include <cstring>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fuse::relight::bridge::host {

namespace {

enum Class : uint32_t {
    kPlain = 0,
    kSkip = 1u << 0,          // never stored
    kTransient = 1u << 1,     // dropped once its frame is presented
    kKeyed = 1u << 2,         // last writer wins per key
    kUnlockRegion = 1u << 4,  // UnlockRect/UnlockBox([face,] [level,] i32[] rect/box, ...): keyed by region
    kUnlockBuffer = 1u << 5,  // VB/IB Unlock(offset, flags, heapChunk, heapBytes, data)
    kBarrier = 1u << 6,
    kDestroy = 1u << 7,
    kCreates = 1u << 8,       // last field is `handle result`
    kEffect = 1u << 9,        // changes device state through its object (StateBlock Apply)
    kRecordMark = 1u << 10,   // BeginStateBlock / EndStateBlock
};

struct Info {
    uint32_t cls = kPlain;
    uint8_t keyFields = 0;   // leading u32 fields in the key
    bool keySize = false;    // payload size is part of the key (shader constants)
    uint8_t lockPrefix = 0;  // bytes of the face/level prefix of UnlockRect/UnlockBox payloads
};

constexpr uint32_t kD3DLockDiscard = 0x00002000u;

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

template <size_t N>
bool oneOf(std::string_view m, const std::array<std::string_view, N>& list) {
    for (std::string_view x : list) {
        if (m == x) {
            return true;
        }
    }
    return false;
}

Info classify(uint16_t id) {
    const std::string_view iface = interfaceName(id);
    const std::string_view m = methodName(id);
    Info info;
    if (std::string_view(schema::commandName(id)) == "Direct3DCreate9") {
        info.cls = kCreates;
        return info;
    }
    if (iface == "Reply") {
        info.cls = kSkip;
        return info;
    }
    if (iface == "Bridge") {
        info.cls = (m == "UnlinkResource" || m == "UnlinkVolumeResource") ? kPlain : kSkip;
        return info;
    }
    if (iface == "RemixApi") {
        return info;  // RL-6.2 refines; kept verbatim
    }
    static constexpr std::array<std::string_view, 13> kGetLinks = {
        "GetSwapChain", "GetBackBuffer", "GetRenderTarget", "GetDepthStencilSurface", "GetTexture",
        "GetVertexDeclaration", "GetVertexShader", "GetStreamSource", "GetIndices", "GetPixelShader",
        "GetSurfaceLevel", "GetVolumeLevel", "GetCubeMapSurface"};
    static constexpr std::array<std::string_view, 18> kTransientNames = {
        "BeginScene", "EndScene", "Clear", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
        "DrawIndexedPrimitiveUP", "DrawPrimitiveUPHeap", "DrawRectPatch", "DrawTriPatch", "ProcessVertices",
        "StretchRect", "ColorFill", "Present", "PresentEx", "Issue", "WaitForVBlank", "ComposeRects"};
    // Capture and CreateStateBlock read the device state; Reset clears it.
    static constexpr std::array<std::string_view, 10> kKey1 = {
        "SetRenderState", "SetTransform", "SetTexture", "SetStreamSource", "SetStreamSourceFreq", "SetLight",
        "LightEnable", "SetClipPlane", "SetRenderTarget", "SetGammaRamp"};
    static constexpr std::array<std::string_view, 2> kKey2 = {"SetTextureStageState", "SetSamplerState"};
    static constexpr std::array<std::string_view, 24> kKey0 = {
        "SetFVF", "SetIndices", "SetVertexDeclaration", "SetVertexShader", "SetPixelShader", "SetViewport",
        "SetMaterial", "SetScissorRect", "SetDepthStencilSurface", "SetCurrentTexturePalette",
        "SetSoftwareVertexProcessing", "SetNPatchMode", "SetClipStatus", "SetMaximumFrameLatency",
        "SetGPUThreadPriority", "SetDialogBoxMode", "ShowCursor", "SetCursorProperties", "SetCursorPosition",
        "SetLOD", "SetPriority", "SetAutoGenFilterType", "SetConvolutionMonoKernel", "AddDirtyRect"};
    // Lock / LockRect / LockBox are read-backs (RL-2.2: the data comes back with the Unlock).
    if (m == "QueryInterface" || m == "Lock" || m == "LockRect" || m == "LockBox" || m == "EvictManagedResources" ||
        m == "PreLoad" || startsWith(m, "Check") || startsWith(m, "Enum") || startsWith(m, "Validate") ||
        startsWith(m, "Is") || m == "TestCooperativeLevel") {
        info.cls = kSkip;
    } else if (startsWith(m, "Get")) {
        info.cls = oneOf(m, kGetLinks) ? kCreates : kSkip;
    } else if (startsWith(m, "Create")) {
        info.cls = kCreates;
        if (m == "CreateStateBlock") {
            info.cls |= kBarrier;
        }
    } else if (m == "Destroy") {
        info.cls = kDestroy;
    } else if (oneOf(m, kTransientNames)) {
        info.cls = kTransient;
    } else if (m == "Capture" || m == "Reset" || m == "ResetEx") {
        info.cls = kBarrier;
    } else if (m == "Apply") {
        info.cls = kEffect;
    } else if (m == "BeginStateBlock") {
        info.cls = kRecordMark;
    } else if (m == "EndStateBlock") {
        info.cls = kRecordMark | kCreates;
    } else if (m == "UnlockRect" || m == "UnlockBox") {
        info.cls = kUnlockRegion;
    } else if (m == "Unlock") {
        info.cls = kUnlockBuffer;
    } else if (oneOf(m, kKey2)) {
        info.cls = kKeyed;
        info.keyFields = 2;
    } else if (oneOf(m, kKey1) || m == "SetPaletteEntries") {
        info.cls = kKeyed;
        info.keyFields = 1;
    } else if (oneOf(m, kKey0)) {
        info.cls = kKeyed;
    } else if (startsWith(m, "SetVertexShaderConstant") || startsWith(m, "SetPixelShaderConstant")) {
        info.cls = kKeyed;
        info.keyFields = 1;
        info.keySize = true;
    }
    if (info.cls & kUnlockRegion) {
        info.lockPrefix = iface == "IDirect3DCubeTexture9" ? 8 : (iface == "IDirect3DTexture9" || iface == "IDirect3DVolumeTexture9") ? 4 : 0;
    }
    return info;
}

const Info& info(uint16_t id) {
    static const std::vector<Info> table = [] {
        std::vector<Info> t(schema::kCommandCount + 1u);
        for (uint32_t i = 1; i <= schema::kCommandCount; ++i) {
            t[i] = classify(static_cast<uint16_t>(i));
        }
        return t;
    }();
    static const Info unknown {};
    return id < table.size() ? table[id] : unknown;
}

uint32_t readU32(const std::vector<uint8_t>& p, size_t offset) {
    uint32_t v = 0;
    if (offset + 4 <= p.size()) {
        std::memcpy(&v, p.data() + offset, 4);
    }
    return v;
}

void appendRaw(std::string& key, const void* p, size_t n) { key.append(static_cast<const char*>(p), n); }

std::string baseKey(char tag, const JournalEntry& e) {
    std::string key(1, tag);
    appendRaw(key, &e.command, sizeof(e.command));
    appendRaw(key, &e.handle, sizeof(e.handle));
    return key;
}

// Key of a compactable entry, or "" when the entry is not superseded by later writes.
std::string entryKey(const JournalEntry& e) {
    const Info& in = info(e.command);
    if (in.cls & kKeyed) {
        std::string key = baseKey('K', e);
        const size_t n = std::min<size_t>(size_t(in.keyFields) * 4, e.payload.size());
        appendRaw(key, e.payload.data(), n);
        if (in.keySize) {
            const uint64_t sz = e.payload.size();
            appendRaw(key, &sz, sizeof(sz));
        }
        return key;
    }
    if (in.cls & kUnlockBuffer) {
        // Unlock: u32 offset, u32 lockFlags, bytes data -> (offset, data size)
        std::string key = baseKey('B', e);
        const uint32_t offset = readU32(e.payload, 0);
        const uint64_t sz = e.payload.size();
        appendRaw(key, &offset, sizeof(offset));
        appendRaw(key, &sz, sizeof(sz));
        return key;
    }
    if (in.cls & kUnlockRegion) {
        // [face,] [level,] i32[] rect/box: the prefix and the counted region array.
        std::string key = baseKey('R', e);
        const size_t count = readU32(e.payload, in.lockPrefix);
        const size_t n = std::min<size_t>(e.payload.size(), in.lockPrefix + 4 + count * 4);
        appendRaw(key, e.payload.data(), n);
        return key;
    }
    return std::string();
}

}  // namespace

CommandJournal::CommandJournal(size_t limitBytes) : limit_(limitBytes) {}

void CommandJournal::push(uint16_t command, uint32_t handle, const uint8_t* data, size_t size) {
    JournalEntry e;
    e.command = command;
    e.handle = handle;
    e.payload.assign(data, data + size);
    bytes_ += size + sizeof(JournalEntry);
    entries_.push_back(std::move(e));
}

bool CommandJournal::append(uint16_t command, uint32_t handle, const uint8_t* data, size_t size) {
    if (!enabled_) {
        return false;
    }
    const Info& in = info(command);
    if (in.cls & kSkip) {
        ++stats_.skipped;
        return true;
    }
    ++stats_.appended;
    push(command, handle, data, size);
    if (isPresentCommand(command)) {
        compact();
    }
    if (bytes_ > limit_) {
        enabled_ = false;
        entries_.clear();
        entries_.shrink_to_fit();
        bytes_ = 0;
        return false;
    }
    return true;
}

void CommandJournal::compact() {
    ++stats_.compactions;
    const size_t n = entries_.size();
    std::vector<uint8_t> keep(n, 1);

    // 1) Transient work of presented frames: everything up to the last Present.
    size_t lastPresent = n;
    for (size_t i = n; i-- > 0;) {
        if (isPresentCommand(entries_[i].command)) {
            lastPresent = i;
            break;
        }
    }
    if (lastPresent != n) {
        for (size_t i = 0; i <= lastPresent; ++i) {
            if (info(entries_[i].command).cls & kTransient) {
                keep[i] = 0;
                ++stats_.droppedTransient;
            }
        }
    }

    // 2) Superseded writes, walking backwards; barriers reset what later writes can supersede, and
    //    state-block recordings are kept verbatim. Only entries before the frame in flight are
    //    dropped, so a partially replayed frame sees the state its draws saw.
    const size_t frameStart = lastPresent == n ? 0 : lastPresent + 1;
    // Keys each recorded state block holds (its recording's Set* keys); blocks from
    // CreateStateBlock have none listed and count as "every key".
    std::unordered_map<uint32_t, std::vector<std::string>> blockKeys;
    {
        size_t begin = n;
        for (size_t i = 0; i < n; ++i) {
            const std::string_view m = methodName(entries_[i].command);
            if (m == "BeginStateBlock") {
                begin = i;
            } else if (m == "EndStateBlock" && begin != n && entries_[i].payload.size() >= 4) {
                std::vector<std::string>& keys = blockKeys[readU32(entries_[i].payload, entries_[i].payload.size() - 4)];
                for (size_t j = begin + 1; j < i; ++j) {
                    std::string k = entryKey(entries_[j]);
                    if (!k.empty()) {
                        keys.push_back(std::move(k));
                    }
                }
                begin = n;
            }
        }
    }
    // Whether the content a Capture(B) stores still matters: an Apply(B) follows before the next
    // Capture(B), or it is B's last Capture and B is not destroyed.
    std::unordered_map<uint32_t, bool> needCapture;
    for (size_t i = 0; i < n; ++i) {
        if (keep[i] && (info(entries_[i].command).cls & kDestroy)) {
            needCapture[entries_[i].handle] = false;
        }
    }
    auto captureNeeded = [&](uint32_t b) {
        auto it = needCapture.find(b);
        return it == needCapture.end() || it->second;
    };
    std::unordered_set<std::string> seen;
    std::unordered_set<uint32_t> discarded;  // buffers re-uploaded with D3DLOCK_DISCARD later
    bool recording = false;
    for (size_t i = n; i-- > 0;) {
        const JournalEntry& e = entries_[i];
        const Info& in = info(e.command);
        const std::string_view m = methodName(e.command);
        if (m == "EndStateBlock") {
            recording = true;
        }
        if (m == "BeginStateBlock") {
            recording = false;
        }
        if (m == "Capture" && keep[i]) {
            if (!captureNeeded(e.handle)) {
                if (i < frameStart) {
                    keep[i] = 0;  // overwritten by a later Capture, or the block dies unapplied
                    ++stats_.droppedSuperseded;
                }
                continue;
            }
            needCapture[e.handle] = false;
            // Capture(B) reads B's keys from the device: older writes of those keys must stay.
            auto it = blockKeys.find(e.handle);
            if (it == blockKeys.end()) {
                seen.clear();
                discarded.clear();
            } else {
                for (const std::string& k : it->second) {
                    seen.erase(k);
                }
            }
            continue;
        }
        if (in.cls & kBarrier) {
            seen.clear();
            discarded.clear();
            continue;
        }
        // Writes of the frame in flight neither drop nor supersede: a draw earlier in that frame may
        // depend on the older value.
        if (recording || !keep[i] || i >= frameStart) {
            continue;
        }
        if (m == "Apply") {
            // Apply(B) writes B's keys: superseded when every one is rewritten later, and it
            // supersedes older writes of them.
            auto it = blockKeys.find(e.handle);
            if (it == blockKeys.end()) {
                needCapture[e.handle] = true;
                continue;
            }
            bool all = true;
            for (const std::string& k : it->second) {
                all = all && seen.count(k) != 0;
            }
            if (all) {
                keep[i] = 0;
                ++stats_.droppedSuperseded;
            } else {
                seen.insert(it->second.begin(), it->second.end());
                needCapture[e.handle] = true;
            }
            continue;
        }
        const std::string key = entryKey(e);
        if (key.empty()) {
            continue;
        }
        // An Unlock with D3DLOCK_DISCARD redefines the whole buffer: only a later DISCARD supersedes it.
        const bool isDiscard = (in.cls & kUnlockBuffer) && (readU32(e.payload, 4) & kD3DLockDiscard) != 0;
        bool superseded = !isDiscard && seen.count(key) != 0;
        if ((in.cls & kUnlockBuffer) && discarded.count(e.handle) != 0) {
            superseded = true;
        }
        if (superseded) {
            keep[i] = 0;
            ++stats_.droppedSuperseded;
            continue;
        }
        seen.insert(key);
        if (isDiscard) {
            discarded.insert(e.handle);
        }
    }
    // 3) Objects created and destroyed inside the journal, with no children and no references left.
    std::set<size_t> destroys;
    for (size_t i = 0; i < n; ++i) {
        if (keep[i] && (info(entries_[i].command).cls & kDestroy)) {
            destroys.insert(i);
        }
    }
    for (size_t d : destroys) {
        const uint32_t h = entries_[d].handle;
        if (h == 0) {
            continue;
        }
        size_t creator = n;
        bool blocked = false;
        for (size_t i = 0; i < d && !blocked; ++i) {
            if (!keep[i]) {
                continue;
            }
            const JournalEntry& e = entries_[i];
            const Info& in = info(e.command);
            const bool createsH = (in.cls & kCreates) && e.payload.size() >= 4 && readU32(e.payload, e.payload.size() - 4) == h;
            if (createsH) {
                if (creator == n) {
                    creator = i;
                }
                if (in.cls & kBarrier) {
                    blocked = true;  // CreateStateBlock captured device state: keep it
                }
                if (e.handle != 0 && info(e.command).cls & kCreates && methodName(e.command).substr(0, 3) == "Get") {
                    blocked = true;  // linked from a parent (GetSurfaceLevel...): the parent owns it
                }
                continue;
            }
            if (e.handle == h) {
                const bool selfConfined = methodName(e.command) == "Capture";  // writes only into h
                if ((in.cls & (kCreates | kEffect)) != 0 || ((in.cls & kBarrier) != 0 && !selfConfined)) {
                    blocked = true;  // produced children, or its effect reached the device (Apply, Reset)
                }
                continue;
            }
            // Any other stored command naming h as a field (small payloads, not bulk data).
            if ((in.cls & (kUnlockBuffer | kUnlockRegion)) == 0 && e.payload.size() <= 64) {
                for (size_t off = 0; off + 4 <= e.payload.size(); ++off) {
                    uint32_t v;
                    std::memcpy(&v, e.payload.data() + off, 4);
                    if (v == h) {
                        blocked = true;
                        break;
                    }
                }
            }
        }
        // References after the Destroy (a stale handle) also keep everything.
        for (size_t i = d + 1; i < n && !blocked; ++i) {
            if (keep[i] && entries_[i].handle == h) {
                blocked = true;
            }
        }
        if (creator == n || blocked) {
            continue;
        }
        // A recorded state block goes with its whole recording (Begin..End are adjacent: nothing
        // inside a recording is ever dropped); the recording must not have created objects.
        size_t first = creator;
        if (info(entries_[creator].command).cls & kRecordMark) {
            size_t b = creator;
            while (b > 0 && !(keep[b - 1] && methodName(entries_[b - 1].command) == "BeginStateBlock")) {
                --b;
                if (keep[b] && (info(entries_[b].command).cls & kCreates)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked || b == 0) {
                continue;
            }
            first = b - 1;
        }
        for (size_t i = first; i <= d; ++i) {
            if (keep[i] && (i <= creator || entries_[i].handle == h)) {
                keep[i] = 0;
                ++stats_.droppedDestroyed;
            }
        }
    }

    std::vector<JournalEntry> out;
    out.reserve(n);
    size_t bytes = 0;
    for (size_t i = 0; i < n; ++i) {
        if (keep[i]) {
            bytes += entries_[i].payload.size() + sizeof(JournalEntry);
            out.push_back(std::move(entries_[i]));
        }
    }
    entries_ = std::move(out);
    bytes_ = bytes;
}

void CommandJournal::clear() {
    entries_.clear();
    bytes_ = 0;
}

}  // namespace fuse::relight::bridge::host
