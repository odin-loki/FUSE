// FUSE Relight RL-2.3: bridge host protocol rules (see protocol.hpp for the upstream notice and
// what they port). Modifications Copyright (c) 2026 FUSE contributors (MIT).
// Ported from dxvk-remix bridge/src/server/main.cpp@0867d3c (which calls answer the client)

#include <fuse/relight/bridge/host/protocol.hpp>

#include <cstring>

namespace fuse::relight::bridge::host {

namespace {

using schema::CommandId;

// Typed reply of a row: the table's "-> Reply_X" suffix (generated schema::replyOf).
ReplyKind queryKind(uint16_t id) {
    switch (static_cast<CommandId>(schema::replyOf(id))) {
    case CommandId::Reply_Result: return ReplyKind::Result;
    case CommandId::Reply_Value: return ReplyKind::Value;
    case CommandId::Reply_Caps: return ReplyKind::Caps;
    case CommandId::Reply_DisplayMode: return ReplyKind::DisplayMode;
    case CommandId::Reply_AdapterIdentifier: return ReplyKind::AdapterIdentifier;
    case CommandId::Reply_RasterStatus: return ReplyKind::RasterStatus;
    case CommandId::Reply_Luid: return ReplyKind::Luid;
    case CommandId::Reply_Data: return ReplyKind::Data;
    default: return ReplyKind::None;
    }
}

struct Tables {
    std::vector<uint8_t> kind;
    std::vector<uint8_t> present;
    std::vector<uint8_t> reply;
    Tables()
        : kind(schema::kCommandCount + 1u, 0), present(schema::kCommandCount + 1u, 0), reply(schema::kCommandCount + 1u, 0) {
        for (uint32_t id = 1; id <= schema::kCommandCount; ++id) {
            kind[id] = static_cast<uint8_t>(queryKind(static_cast<uint16_t>(id)));
            const std::string_view m = methodName(static_cast<uint16_t>(id));
            present[id] = (interfaceName(static_cast<uint16_t>(id)) != "Reply" && (m == "Present" || m == "PresentEx")) ? 1 : 0;
            reply[id] = interfaceName(static_cast<uint16_t>(id)) == "Reply" ? 1 : 0;
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

template <class R>
std::vector<uint8_t> encodeReply(int32_t hr) {
    R r {};
    r.hresult = hr;
    std::vector<uint8_t> b(schema::cmd::encodedSize(r));
    ipc::WireWriter w(b.data(), b.size());
    schema::cmd::encode(w, r);
    return b;
}

}  // namespace

std::string_view methodName(uint16_t command) noexcept {
    const std::string_view name = schema::commandName(command);
    const size_t us = name.find('_');
    return us == std::string_view::npos ? name : name.substr(us + 1);
}

std::string_view interfaceName(uint16_t command) noexcept {
    const std::string_view name = schema::commandName(command);
    const size_t us = name.find('_');
    return us == std::string_view::npos ? std::string_view() : name.substr(0, us);
}

ReplyKind replyKind(uint16_t command, uint16_t flags) noexcept {
    if (!schema::isKnownCommand(command)) {
        return ReplyKind::None;
    }
    const auto k = static_cast<ReplyKind>(tables().kind[command]);
    if (k == ReplyKind::None && (flags & kWantReply) != 0 && tables().reply[command] == 0) {
        return ReplyKind::Result;
    }
    return k;
}

uint16_t replyCommand(ReplyKind kind) noexcept {
    switch (kind) {
    case ReplyKind::None: return 0;
    case ReplyKind::Result: return static_cast<uint16_t>(CommandId::Reply_Result);
    case ReplyKind::Value: return static_cast<uint16_t>(CommandId::Reply_Value);
    case ReplyKind::Caps: return static_cast<uint16_t>(CommandId::Reply_Caps);
    case ReplyKind::DisplayMode: return static_cast<uint16_t>(CommandId::Reply_DisplayMode);
    case ReplyKind::AdapterIdentifier: return static_cast<uint16_t>(CommandId::Reply_AdapterIdentifier);
    case ReplyKind::RasterStatus: return static_cast<uint16_t>(CommandId::Reply_RasterStatus);
    case ReplyKind::Luid: return static_cast<uint16_t>(CommandId::Reply_Luid);
    case ReplyKind::Data: return static_cast<uint16_t>(CommandId::Reply_Data);
    }
    return 0;
}

bool isReplyCommand(uint16_t command) noexcept { return schema::isKnownCommand(command) && tables().reply[command] != 0; }

std::vector<uint8_t> encodeDefaultReply(ReplyKind kind, int32_t hresult) {
    namespace c = schema::cmd;
    switch (kind) {
    case ReplyKind::None: return {};
    case ReplyKind::Result: return encodeReply<c::Reply_Result>(hresult);
    case ReplyKind::Value: return encodeReply<c::Reply_Value>(hresult);
    case ReplyKind::Caps: return encodeReply<c::Reply_Caps>(hresult);
    case ReplyKind::DisplayMode: return encodeReply<c::Reply_DisplayMode>(hresult);
    case ReplyKind::AdapterIdentifier: return encodeReply<c::Reply_AdapterIdentifier>(hresult);
    case ReplyKind::RasterStatus: return encodeReply<c::Reply_RasterStatus>(hresult);
    case ReplyKind::Luid: return encodeReply<c::Reply_Luid>(hresult);
    case ReplyKind::Data: return encodeReply<c::Reply_Data>(hresult);
    }
    return {};
}

void patchReplyUid(std::vector<uint8_t>& encodedReply, uint32_t uid) noexcept {
    if (encodedReply.size() >= 4) {
        std::memcpy(encodedReply.data(), &uid, 4);
    }
}

bool isPresentCommand(uint16_t command) noexcept {
    return schema::isKnownCommand(command) && tables().present[command] != 0;
}

}  // namespace fuse::relight::bridge::host
