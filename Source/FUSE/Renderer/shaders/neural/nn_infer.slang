// WP-9.1 "neural.infer", portable kernel. Slang twin of nn_infer.comp (see there); compiled with -fp-mode precise.
#include "nn_common.slang"

[[vk::push_constant]] ConstantBuffer<NeuralPush> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint s = tid.x;
    if (s >= pc.count) {
        return;
    }
    const NeuralGpuHeader h = ((NeuralGpuHeader*)pc.header)[0];
    float* inputs = (float*)pc.inputs;
    float* outputs = (float*)pc.outputs;
    float x[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (uint d = 0; d < h.inputDims; ++d) {
        x[d] = inputs[s * h.inputDims + d];
    }
    float a[kNnMaxWidth];
    float b[kNnMaxWidth];
    nnEncode(h, pc.params, x, a);
    for (uint j = 0; j < h.layerCount; ++j) {
        const uint nin = h.layerIn[j];
        const uint nout = h.layerOut[j];
        const uint act = j + 1u == h.layerCount ? h.outputActivation : h.hiddenActivation;
        for (uint o = 0; o < nout; ++o) {
            float acc = nnParam(pc.params, h.paramPrecision, h.biasOffset[j] + o);
            const uint row = h.weightOffset[j] + o * nin;
            for (uint i = 0; i < nin; ++i) {
                acc = acc + nnParam(pc.params, h.paramPrecision, row + i) * a[i];
            }
            b[o] = nnActivation(act, acc, h.leakySlope);
        }
        for (uint o = 0; o < nout; ++o) {
            a[o] = b[o];
        }
    }
    for (uint o = 0; o < h.outputDims; ++o) {
        outputs[s * h.outputDims + o] = a[o];
    }
}
