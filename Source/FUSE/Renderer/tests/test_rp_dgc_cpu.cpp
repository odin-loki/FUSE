// WP-9.3 device-generated commands: CPU gates (stub-safe).
//
//   layout      DgcSequence / DgcPush / DgcDrawIndexed sizes and offsets; the push blocks of
//               dgc_generate.comp and dgc_generate.slang declare the same fields in the same order
//               with the std430 offsets of the C++ struct; the kernels' GpuInstance word constants
//               equal gpu_scene_types.hpp.
//   reference   dgc_generate_reference: sequence i == culled draw i with the bucket of its material,
//               bucket args partition the draws, maxDraws clamp, default bucket for unknown materials.
//   emulation   CPU emulation of the indirect-commands layout over the reference sequences produces
//               exactly the indirect-count path's draw list (same (pipeline, draw) multiset); binds
//               are elided when the pipeline does not change; short streams are rejected.
//   capability  dgc_evaluate_capability reports each unmet requirement with its reason.
//   commands    CPU command-count model: DGC records 2 commands, the fallback 2 x buckets.
//   api         DgcPipelineSelector without a device fails with a reason (NoDevice / NoVulkanBackend).
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/vk/dgc/dgc_pipeline_selector.hpp>
#include <fuse/renderer/vk/dgc/dgc_reference.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace fuse::renderer::dgc;
using fuse::i32;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

namespace {
int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string readText(const std::string& path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

struct Field {
    std::string type;
    std::string name;
};

/// Fields of the first `{ ... }` block after `marker`: "type name;" lines.
std::vector<Field> pushFields(const std::string& text, const std::string& marker) {
    std::vector<Field> out;
    const usize at = text.find(marker);
    if (at == std::string::npos) {
        return out;
    }
    const usize open = text.find('{', at);
    const usize close = text.find('}', open);
    std::istringstream body(text.substr(open + 1, close - open - 1));
    std::string line;
    while (std::getline(body, line)) {
        const usize comment = line.find("//");
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        std::istringstream ls(line);
        Field f;
        if (ls >> f.type >> f.name) {
            if (!f.name.empty() && f.name.back() == ';') {
                f.name.pop_back();
            }
            out.push_back(f);
        }
    }
    return out;
}

/// std430 offsets of scalar fields (uint64_t: 8, uint: 4).
std::vector<u32> offsets(const std::vector<Field>& fields, bool& ok) {
    std::vector<u32> out;
    u32 at = 0;
    for (const Field& f : fields) {
        u32 size = 0;
        if (f.type == "uint64_t") {
            size = 8;
        } else if (f.type == "uint") {
            size = 4;
        } else {
            ok = false;
        }
        at = (at + size - 1u) / size * size;
        out.push_back(at);
        at += size;
    }
    return out;
}

u32 findConstant(const std::string& text, const std::string& name) {
    const usize at = text.find(name);
    if (at == std::string::npos) {
        return ~0u;
    }
    const usize eq = text.find('=', at);
    return static_cast<u32>(std::strtoul(text.c_str() + eq + 1, nullptr, 10));
}

int runLayout() {
    expect(sizeof(DgcSequence) == kDgcSequenceStride, "DgcSequence stride");
    expect(offsetof(DgcSequence, draw) == kDgcTokenDrawIndexedOffset, "DRAW_INDEXED token offset");
    expect(offsetof(DgcSequence, pipelineIndex) == kDgcTokenExecutionSetOffset, "EXECUTION_SET token offset");
    expect(sizeof(DgcDrawIndexed) == 20u, "VkDrawIndexedIndirectCommand");
    expect(kDgcTokenDrawIndexedOffset % 4u == 0u && kDgcSequenceStride % 4u == 0u, "token alignment");
    const u32 expected[13] = {
        static_cast<u32>(offsetof(DgcPush, args)),          static_cast<u32>(offsetof(DgcPush, drawCount)),
        static_cast<u32>(offsetof(DgcPush, scene)),         static_cast<u32>(offsetof(DgcPush, materialBuckets)),
        static_cast<u32>(offsetof(DgcPush, sequences)),     static_cast<u32>(offsetof(DgcPush, bucketArgs)),
        static_cast<u32>(offsetof(DgcPush, counts)),        static_cast<u32>(offsetof(DgcPush, maxDraws)),
        static_cast<u32>(offsetof(DgcPush, bucketCount)),   static_cast<u32>(offsetof(DgcPush, materialCount)),
        static_cast<u32>(offsetof(DgcPush, defaultBucket)), static_cast<u32>(offsetof(DgcPush, flags)),
        static_cast<u32>(offsetof(DgcPush, pad))};
    const char* names[13] = {"args", "drawCount", "scene", "materialBuckets", "sequences", "bucketArgs", "counts",
                             "maxDraws", "bucketCount", "materialCount", "defaultBucket", "flags", "pad"};
    const std::string dir = FUSE_RP_DGC_SHADER_DIR;
    for (const char* file : {"/dgc_generate.comp", "/dgc_generate.slang"}) {
        const std::string text = readText(dir + file);
        expect(!text.empty(), "kernel source readable");
        const std::vector<Field> fields =
            pushFields(text, std::strstr(file, ".comp") != nullptr ? "uniform Push" : "struct Push");
        bool ok = true;
        const std::vector<u32> offs = offsets(fields, ok);
        expect(ok && fields.size() == 13u, "push block: 13 scalar fields");
        for (usize i = 0; i < fields.size() && i < 13u; ++i) {
            if (fields[i].name != names[i] || offs[i] != expected[i]) {
                std::fprintf(stderr, "FAIL: %s push field %zu: %s @%u, C++ %s @%u\n", file, i, fields[i].name.c_str(),
                             offs[i], names[i], expected[i]);
                ++g_failures;
            }
        }
        expect(findConstant(text, "kGpuInstanceWords") == sizeof(fuse::renderer::gpu_scene::GpuInstance) / 4u,
               "kGpuInstanceWords == sizeof(GpuInstance) / 4");
        expect(findConstant(text, "kGpuInstanceMaterial") ==
                   offsetof(fuse::renderer::gpu_scene::GpuInstance, material) / 4u,
               "kGpuInstanceMaterial == offsetof(GpuInstance, material) / 4");
        expect(text.find("local_size_x = 64") != std::string::npos || text.find("numthreads(64") != std::string::npos,
               "workgroup 64 == kDgcWorkgroup");
    }
    std::printf("layout: DgcSequence 24 B, DgcPush 80 B, GLSL + Slang push blocks match\n");
    return g_failures == 0 ? 0 : 1;
}

struct Scenario {
    std::vector<DgcDrawIndexed> draws;
    std::vector<u32> materials; ///< by slot
    std::vector<u32> buckets;   ///< by material
};

Scenario makeScenario(u32 draws, u32 slots, u32 materials, u32 bucketCount, u32 seed) {
    std::mt19937 rng(seed);
    Scenario s;
    for (u32 i = 0; i < slots; ++i) {
        s.materials.push_back(rng() % (materials + 2u)); // some rows out of range
    }
    for (u32 m = 0; m < materials; ++m) {
        s.buckets.push_back(rng() % (bucketCount + 1u)); // some buckets out of range
    }
    for (u32 i = 0; i < draws; ++i) {
        DgcDrawIndexed d{};
        d.indexCount = 36u;
        d.instanceCount = 1u;
        d.firstIndex = (rng() % 4u) * 36u;
        d.vertexOffset = static_cast<i32>(rng() % 3u);
        d.firstInstance = rng() % (slots + 3u); // some slots above the table
        s.draws.push_back(d);
    }
    return s;
}

DgcReferenceInput inputOf(const Scenario& s, u32 drawCount, u32 maxDraws, u32 bucketCount, u32 defaultBucket) {
    DgcReferenceInput in{};
    in.draws = s.draws.data();
    in.drawCount = drawCount;
    in.maxDraws = maxDraws;
    in.instanceMaterials = s.materials.data();
    in.instanceCount = static_cast<u32>(s.materials.size());
    in.materialBuckets = s.buckets.data();
    in.materialCount = static_cast<u32>(s.buckets.size());
    in.bucketCount = bucketCount;
    in.defaultBucket = defaultBucket;
    return in;
}

int runReference() {
    for (u32 trial = 0; trial < 20u; ++trial) {
        const u32 bucketCount = 1u + trial % 8u;
        const u32 drawCount = 50u + trial * 37u;
        const u32 maxDraws = trial % 3u == 0u ? drawCount / 2u : drawCount + 5u;
        const u32 defaultBucket = trial % bucketCount;
        const Scenario s = makeScenario(drawCount, 400u, 12u, bucketCount, 77u + trial);
        DgcReferenceOutput out;
        dgc_generate_reference(inputOf(s, drawCount, maxDraws, bucketCount, defaultBucket), out);
        const u32 n = std::min(drawCount, maxDraws);
        expect(out.sequences.size() == n, "sequence count == min(draws, maxDraws)");
        std::vector<u32> perBucket(bucketCount, 0u);
        for (u32 i = 0; i < n && i < out.sequences.size(); ++i) {
            const DgcSequence& q = out.sequences[i];
            const DgcDrawIndexed& d = s.draws[i];
            const u32 slot = d.firstInstance;
            const u32 material = slot < s.materials.size() ? s.materials[slot] : 0xFFFFFFFFu;
            u32 bucket = material < s.buckets.size() ? s.buckets[material] : defaultBucket;
            bucket = bucket < bucketCount ? bucket : defaultBucket;
            expect(q.pipelineIndex == bucket, "sequence bucket == material bucket");
            expect(std::memcmp(&q.draw, &d, sizeof(d)) == 0, "sequence draw == culled draw i");
            ++perBucket[bucket];
        }
        u32 total = 0;
        for (u32 b = 0; b < bucketCount; ++b) {
            expect(out.bucketArgs[b].size() == perBucket[b], "bucket args partition the draws");
            total += static_cast<u32>(out.bucketArgs[b].size());
        }
        expect(total == n, "every draw in exactly one bucket");
    }
    std::printf("reference: 20 scenarios (1..8 buckets, maxDraws clamp, out-of-range materials / buckets / slots)\n");
    return g_failures == 0 ? 0 : 1;
}

int runEmulation() {
    u32 totalDraws = 0;
    for (u32 trial = 0; trial < 20u; ++trial) {
        const u32 bucketCount = 1u + trial % 8u;
        const u32 drawCount = 64u + trial * 53u;
        const Scenario s = makeScenario(drawCount, 300u, 10u, bucketCount, 991u + trial);
        DgcReferenceOutput out;
        dgc_generate_reference(inputOf(s, drawCount, drawCount, bucketCount, 0u), out);
        const std::vector<u8> stream(reinterpret_cast<const u8*>(out.sequences.data()),
                                     reinterpret_cast<const u8*>(out.sequences.data() + out.sequences.size()));
        std::vector<DgcCommand> dgcStream;
        expect(dgc_emulate_layout(kDgcLayoutTokens, kDgcLayoutTokenCount, kDgcSequenceStride, stream.data(), stream.size(),
                                  static_cast<u32>(out.sequences.size()), drawCount, 0u, dgcStream),
               "emulation reads the stream");
        std::vector<u32> counts;
        for (const auto& b : out.bucketArgs) {
            counts.push_back(static_cast<u32>(b.size()));
        }
        std::vector<DgcCommand> icStream;
        dgc_indirect_count_commands(out.bucketArgs, counts.data(), icStream);
        expect(dgc_same_draws(dgcStream, icStream), "emulated DGC draws == indirect-count draw list");
        // Binds: one per pipeline change (initial pipeline 0 is bound before the execute).
        u32 changes = 0;
        u32 bound = 0;
        for (const DgcSequence& q : out.sequences) {
            if (q.pipelineIndex != bound) {
                ++changes;
                bound = q.pipelineIndex;
            }
        }
        u32 binds = 0;
        for (const DgcCommand& c : dgcStream) {
            binds += c.kind == DgcCommand::Kind::BindPipeline ? 1u : 0u;
        }
        expect(binds == changes, "EXECUTION_SET binds only on change");
        // Every draw runs with the pipeline of its sequence.
        std::vector<DgcResolvedDraw> resolved;
        dgc_resolve_draws(dgcStream, resolved);
        for (usize i = 0; i < resolved.size() && i < out.sequences.size(); ++i) {
            expect(resolved[i].pipeline == out.sequences[i].pipelineIndex, "draw i runs with sequence i's pipeline");
        }
        totalDraws += static_cast<u32>(resolved.size());
        // maxSequences clamp and short streams.
        std::vector<DgcCommand> clamped;
        dgc_emulate_layout(kDgcLayoutTokens, kDgcLayoutTokenCount, kDgcSequenceStride, stream.data(), stream.size(),
                           static_cast<u32>(out.sequences.size()), 10u, 0u, clamped);
        std::vector<DgcResolvedDraw> clampedDraws;
        dgc_resolve_draws(clamped, clampedDraws);
        expect(clampedDraws.size() == std::min<usize>(10u, out.sequences.size()), "maxSequences clamps");
        std::vector<DgcCommand> shortRun;
        expect(!dgc_emulate_layout(kDgcLayoutTokens, kDgcLayoutTokenCount, kDgcSequenceStride, stream.data(),
                                   stream.size() - 1u, static_cast<u32>(out.sequences.size()), drawCount, 0u, shortRun),
               "short stream rejected");
    }
    // Negative control: a changed bucket makes the lists differ.
    const Scenario s = makeScenario(40u, 50u, 6u, 4u, 5u);
    DgcReferenceOutput out;
    dgc_generate_reference(inputOf(s, 40u, 40u, 4u, 0u), out);
    out.sequences[7].pipelineIndex = (out.sequences[7].pipelineIndex + 1u) % 4u;
    std::vector<DgcCommand> a;
    dgc_emulate_layout(kDgcLayoutTokens, kDgcLayoutTokenCount, kDgcSequenceStride, out.sequences.data(),
                       out.sequences.size() * sizeof(DgcSequence), 40u, 40u, 0u, a);
    std::vector<u32> counts;
    for (const auto& b : out.bucketArgs) {
        counts.push_back(static_cast<u32>(b.size()));
    }
    std::vector<DgcCommand> b;
    dgc_indirect_count_commands(out.bucketArgs, counts.data(), b);
    expect(!dgc_same_draws(a, b), "negative control: a wrong bucket is detected");
    std::printf("emulation: 20 scenarios, %u draws: emulated DGC stream == indirect-count draw list\n", totalDraws);
    return g_failures == 0 ? 0 : 1;
}

int runCapability() {
    DgcCapabilityInputs ok{};
    ok.vulkanBackend = ok.deviceValid = ok.bufferDeviceAddress = ok.drawIndirectCount = true;
    ok.extensionEnabled = ok.maintenance5Enabled = ok.featureStateKnown = ok.featureEnabled = ok.entryPoints = true;
    ok.maxIndirectPipelineCount = 4096;
    ok.maxIndirectSequenceCount = 1u << 20;
    ok.maxIndirectCommandsTokenCount = 16;
    ok.maxIndirectCommandsTokenOffset = 2047;
    ok.maxIndirectCommandsIndirectStride = 2048;
    ok.pipelineBindingStages = 0x11u;
    ok.bucketCount = 8;
    ok.maxDraws = 10000;
    expect(dgc_evaluate_capability(ok) == DgcReason::Supported, "all requirements met");
    struct Case {
        void (*mutate)(DgcCapabilityInputs&);
        DgcReason reason;
    };
    const Case cases[] = {
        {[](DgcCapabilityInputs& c) { c.bucketCount = 0; }, DgcReason::BadBucketCount},
        {[](DgcCapabilityInputs& c) { c.bucketCount = kDgcMaxBuckets + 1u; }, DgcReason::BadBucketCount},
        {[](DgcCapabilityInputs& c) { c.forceFallback = true; }, DgcReason::ForcedFallback},
        {[](DgcCapabilityInputs& c) { c.vulkanBackend = false; }, DgcReason::NoVulkanBackend},
        {[](DgcCapabilityInputs& c) { c.deviceValid = false; }, DgcReason::NoDevice},
        {[](DgcCapabilityInputs& c) { c.bufferDeviceAddress = false; }, DgcReason::NoBufferDeviceAddress},
        {[](DgcCapabilityInputs& c) { c.drawIndirectCount = false; }, DgcReason::NoDrawIndirectCount},
        {[](DgcCapabilityInputs& c) { c.extensionEnabled = false; }, DgcReason::ExtensionNotEnabled},
        {[](DgcCapabilityInputs& c) { c.maintenance5Enabled = false; }, DgcReason::Maintenance5NotEnabled},
        {[](DgcCapabilityInputs& c) { c.featureStateKnown = false; }, DgcReason::FeatureStateUnknown},
        {[](DgcCapabilityInputs& c) { c.featureEnabled = false; }, DgcReason::FeatureNotEnabled},
        {[](DgcCapabilityInputs& c) { c.entryPoints = false; }, DgcReason::EntryPointsMissing},
        {[](DgcCapabilityInputs& c) { c.pipelineBindingStages = 0x20u; }, DgcReason::StagesUnsupported},
        {[](DgcCapabilityInputs& c) { c.pipelineBindingStages = 0x01u; }, DgcReason::StagesUnsupported},
        {[](DgcCapabilityInputs& c) { c.maxIndirectPipelineCount = 7; }, DgcReason::TooManyPipelines},
        {[](DgcCapabilityInputs& c) { c.maxIndirectSequenceCount = 9999; }, DgcReason::TooManySequences},
        {[](DgcCapabilityInputs& c) { c.maxIndirectCommandsTokenCount = 1; }, DgcReason::LayoutLimits},
        {[](DgcCapabilityInputs& c) { c.maxIndirectCommandsTokenOffset = 3; }, DgcReason::LayoutLimits},
        {[](DgcCapabilityInputs& c) { c.maxIndirectCommandsIndirectStride = 20; }, DgcReason::LayoutLimits},
    };
    for (const Case& c : cases) {
        DgcCapabilityInputs in = ok;
        c.mutate(in);
        const DgcReason r = dgc_evaluate_capability(in);
        if (r != c.reason) {
            std::fprintf(stderr, "FAIL: capability: got '%s', expected '%s'\n", dgc_reason_text(r), dgc_reason_text(c.reason));
            ++g_failures;
        }
        expect(std::strlen(dgc_reason_text(r)) > 8u, "reason text");
    }
    // The local-SDK situation: extension enabled, feature chain not handed over.
    DgcCapabilityInputs local = ok;
    local.featureStateKnown = false;
    local.featureEnabled = false;
    std::printf("capability: %zu cases; local SDK without a feature chain -> '%s'\n", sizeof(cases) / sizeof(cases[0]),
                dgc_reason_text(dgc_evaluate_capability(local)));
    return g_failures == 0 ? 0 : 1;
}

int runCommands() {
    for (u32 b = 1; b <= kDgcMaxBuckets; ++b) {
        expect(dgc_recorded_commands(true, b) == 2u, "DGC: 2 commands for any bucket count");
        expect(dgc_recorded_commands(false, b) == 2u * b, "fallback: 2 per bucket");
        if (b >= 2u) {
            expect(dgc_recorded_commands(true, b) < dgc_recorded_commands(false, b), "DGC records fewer commands");
        }
    }
    std::printf("commands: DGC 2 vs fallback 2 x buckets (8 buckets: 2 vs 16)\n");
    return g_failures == 0 ? 0 : 1;
}

int runApi() {
    DgcPipelineSelector selector;
    DgcSelectorDesc desc{};
    desc.bucketCount = 4;
    expect(!selector.init(desc), "init without a device fails");
    expect(!selector.valid() && !selector.dgcActive(), "invalid selector");
    expect(selector.reason() == DgcReason::NoDevice || selector.reason() == DgcReason::NoVulkanBackend,
           "reason: no device / no backend");
    std::printf("api: init without device -> '%s'\n", selector.reasonText());
    desc.bucketCount = 0;
    DgcPipelineSelector bad;
    expect(!bad.init(desc), "bucketCount 0 rejected");
    return g_failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (suite == "layout" || suite == "all") {
        rc |= runLayout();
    }
    if (suite == "reference" || suite == "all") {
        rc |= runReference();
    }
    if (suite == "emulation" || suite == "all") {
        rc |= runEmulation();
    }
    if (suite == "capability" || suite == "all") {
        rc |= runCapability();
    }
    if (suite == "commands" || suite == "all") {
        rc |= runCommands();
    }
    if (suite == "api" || suite == "all") {
        rc |= runApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return rc;
}
