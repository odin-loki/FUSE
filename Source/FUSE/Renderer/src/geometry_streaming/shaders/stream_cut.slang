// WP-5.3 "stream.cut". Slang twin of stream_cut.comp (see there); compiled with -fp-mode precise.
struct StreamCutPush { // StreamCutPush (geometry_streaming.hpp), 64 bytes
    float camX;
    float camY;
    float camZ;
    float errorScale;
    float threshold;
    float znear;
    uint clusterCount;
    uint pageCount;
    uint links;
    uint info;
    uint state;
    uint feedback;
    uint cut;
    uint drawList;
    uint pad0;
    uint pad1;
};
[[vk::push_constant]] ConstantBuffer<StreamCutPush> pc;

[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_stream_words[];

static const uint kPageNone = 0xFFFFFFFFu;
static const uint kPriorityMin = 1u;
static const uint kPriorityLevels = 30u;
static const uint kFeedbackHeader = 4u;

uint handleIndex(uint handle) { return handle & 0xFFFFFu; }

float linkFloat(uint i) { return asfloat(fuse_stream_words[handleIndex(pc.links)][i]); }

bool lodAcceptable(uint b) {
    const float error = linkFloat(b + 4u);
    if (!(error < asfloat(0x7F7FFFFFu))) {
        return false;
    }
    const float dx = linkFloat(b + 0u) - pc.camX;
    const float dy = linkFloat(b + 1u) - pc.camY;
    const float dz = linkFloat(b + 2u) - pc.camZ;
    const float d = sqrt(dx * dx + dy * dy + dz * dz) - linkFloat(b + 3u);
    const float dist = d > pc.znear ? d : pc.znear;
    const float lhs = error * pc.errorScale;
    const float rhs = pc.threshold * dist;
    return lhs <= rhs;
}

uint refinePriority(uint b) {
    if (!(linkFloat(b + 4u) < asfloat(0x7F7FFFFFu))) {
        return kPriorityMin + kPriorityLevels;
    }
    const float dx = linkFloat(b + 0u) - pc.camX;
    const float dy = linkFloat(b + 1u) - pc.camY;
    const float dz = linkFloat(b + 2u) - pc.camZ;
    const float d = sqrt(dx * dx + dy * dy + dz * dz) - linkFloat(b + 3u);
    const float dist = d > pc.znear ? d : pc.znear;
    const float lhs = linkFloat(b + 4u) * pc.errorScale;
    float rhs = pc.threshold * dist;
    uint k = 0u;
    while (k < kPriorityLevels && lhs > rhs) {
        rhs = rhs * 2.0;
        ++k;
    }
    return kPriorityMin + (k > 0u ? k : 1u);
}

bool pageResident(uint page) {
    return page != kPageNone && ((fuse_stream_words[handleIndex(pc.state)][page >> 5u] >> (page & 31u)) & 1u) != 0u;
}

void submitPage(uint page, uint priority) {
    const uint fb = handleIndex(pc.feedback);
    uint old = 0u;
    InterlockedMax(fuse_stream_words[fb][kFeedbackHeader + page], priority, old);
    if (old == 0u) {
        uint slot = 0u;
        InterlockedAdd(fuse_stream_words[fb][0], 1u, slot);
        if (slot < pc.pageCount) {
            fuse_stream_words[fb][kFeedbackHeader + pc.pageCount + slot] = page;
        }
    }
}

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint c = tid.x;
    if (c >= pc.clusterCount) {
        return;
    }
    const uint link = c * 12u;
    const uint info = handleIndex(pc.info);
    const uint memberPage = fuse_stream_words[info][c * 2u + 0u];
    const uint producerPage = fuse_stream_words[info][c * 2u + 1u];
    const bool selfAcc = lodAcceptable(link);
    const bool producerOk = selfAcc || !pageResident(producerPage);
    const bool parentAcc = lodAcceptable(link + 5u);
    const bool inCut = producerOk && !parentAcc && pageResident(memberPage);
    fuse_stream_words[handleIndex(pc.cut)][c] = inCut ? 1u : 0u;
    if (!inCut) {
        return;
    }
    uint slot = 0u;
    InterlockedAdd(fuse_stream_words[handleIndex(pc.feedback)][1], 1u, slot);
    if (slot < pc.clusterCount) {
        fuse_stream_words[handleIndex(pc.drawList)][slot] = c;
    }
    submitPage(memberPage, refinePriority(link + 5u));
    if (!selfAcc && producerPage != kPageNone) {
        submitPage(producerPage, refinePriority(link));
    }
}
