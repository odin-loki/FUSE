#include <fuse/renderer/draw_list.hpp>
#include <fuse/renderer/resource_manager.hpp>

#include <algorithm>

namespace fuse::renderer {

void DrawList::reset() {
    m_calls.clear();
}

bool DrawList::push(const DrawCall& call) {
    if (call.indexCount == 0 || call.instanceCount == 0) {
        return false;
    }
    m_calls.push_back(call);
    return true;
}

u32 DrawList::count() const {
    return static_cast<u32>(m_calls.size());
}

const DrawCall* DrawList::data() const {
    if (m_calls.empty()) {
        return nullptr;
    }
    return m_calls.data();
}

const DrawCall& DrawList::at(u32 index) const {
    if (index >= static_cast<u32>(m_calls.size())) {
        static const DrawCall kEmpty{};
        return kEmpty;
    }
    return m_calls[index];
}

u32 DrawList::totalIndexCount() const {
    u32 total = 0;
    for (const DrawCall& call : m_calls) {
        total += call.indexCount * call.instanceCount;
    }
    return total;
}

void DrawList::sortByMaterial() {
    std::stable_sort(m_calls.begin(), m_calls.end(), [](const DrawCall& a, const DrawCall& b) {
        return a.materialId < b.materialId;
    });
}

u32 DrawList::record(CommandBufferRecorder& recorder) const {
    return record(recorder, nullptr);
}

u32 DrawList::record(CommandBufferRecorder& recorder, const ResourceManager* resources) const {
    if (!recorder.isRecording()) {
        return 0;
    }
    for (const DrawCall& call : m_calls) {
        void* vertexNative = nullptr;
        void* indexNative = nullptr;
        if (resources != nullptr) {
            const Buffer* vertex = resources->getBuffer(call.vertexBuffer);
            if (vertex != nullptr) {
                vertexNative = vertex->handle;
            }
            const Buffer* index = resources->getBuffer(call.indexBuffer);
            if (index != nullptr) {
                indexNative = index->handle;
            }
        }
        recorder.drawIndexed(call.indexCount, call.instanceCount, call.firstIndex,
                             static_cast<i32>(call.vertexOffset), call.materialId, vertexNative,
                             indexNative);
    }
    return count();
}

} // namespace fuse::renderer
