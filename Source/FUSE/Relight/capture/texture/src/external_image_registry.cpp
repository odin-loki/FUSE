// FUSE Relight RL-1.4: external (DXVK-owned) image registry (see external_image_registry.hpp).
#include <fuse/relight/capture/texture/external_image_registry.hpp>

#include <utility>

namespace fuse::relight::capture::texture {

ExternalImageHandle ExternalImageRegistry::registerImage(const ExternalImageInfo& info) {
    if (info.vkImage == 0) {
        return {};
    }
    std::optional<std::pair<ExternalImageHandle, ExternalImageInfo>> released;
    ExternalImageHandle handle;
    ReleaseListener listener;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_byTexture.find(info.texture);
        if (it != m_byTexture.end()) {
            Slot& slot = m_slots[it->second - 1];
            if (slot.info.vkImage == info.vkImage) {
                slot.info = info;
                return {it->second, slot.generation};
            }
            const std::uint32_t index = it->second;
            const std::uint32_t generation = slot.generation;
            released.emplace(ExternalImageHandle{index, generation}, releaseSlotLocked(index));
        }
        std::uint32_t index = 0;
        if (!m_free.empty()) {
            index = m_free.back();
            m_free.pop_back();
        } else {
            m_slots.emplace_back();
            index = std::uint32_t(m_slots.size());
        }
        Slot& slot = m_slots[index - 1];
        ++slot.generation;
        slot.live = true;
        slot.info = info;
        m_byTexture[info.texture] = index;
        ++m_live;
        handle = {index, slot.generation};
        listener = m_onRelease;
    }
    if (released && listener) {
        listener(released->first, released->second);
    }
    return handle;
}

ExternalImageInfo ExternalImageRegistry::releaseSlotLocked(std::uint32_t index) {
    Slot& slot = m_slots[index - 1];
    ExternalImageInfo info = std::move(slot.info);
    slot.info = {};
    slot.live = false;
    // The generation bumps again when the slot is reused, so the released handle stays stale
    // even before that; bump now as well so lookups of it fail immediately.
    ++slot.generation;
    m_byTexture.erase(info.texture);
    m_free.push_back(index);
    --m_live;
    return info;
}

bool ExternalImageRegistry::release(tap::ResourceId texture) {
    ExternalImageHandle handle;
    ExternalImageInfo info;
    ReleaseListener listener;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_byTexture.find(texture);
        if (it == m_byTexture.end()) {
            return false;
        }
        const std::uint32_t index = it->second;
        handle = {index, m_slots[index - 1].generation};
        info = releaseSlotLocked(index);
        listener = m_onRelease;
    }
    if (listener) {
        listener(handle, info);
    }
    return true;
}

void ExternalImageRegistry::releaseAll() {
    std::vector<std::pair<ExternalImageHandle, ExternalImageInfo>> released;
    ReleaseListener listener;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::uint32_t i = 1; i <= m_slots.size(); ++i) {
            if (m_slots[i - 1].live) {
                const ExternalImageHandle handle{i, m_slots[i - 1].generation};
                released.emplace_back(handle, releaseSlotLocked(i));
            }
        }
        listener = m_onRelease;
    }
    if (listener) {
        for (const auto& [handle, info] : released) {
            listener(handle, info);
        }
    }
}

bool ExternalImageRegistry::updateHashes(tap::ResourceId texture, hash::Hash64 imageHash,
                                         hash::Hash64 descriptorHash) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_byTexture.find(texture);
    if (it == m_byTexture.end()) {
        return false;
    }
    ExternalImageInfo& info = m_slots[it->second - 1].info;
    info.imageHash = imageHash;
    info.descriptorHash = descriptorHash;
    return true;
}

std::optional<ExternalImageInfo> ExternalImageRegistry::lookup(ExternalImageHandle handle) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!handle.valid() || handle.index > m_slots.size()) {
        return std::nullopt;
    }
    const Slot& slot = m_slots[handle.index - 1];
    if (!slot.live || slot.generation != handle.generation) {
        return std::nullopt;
    }
    return slot.info;
}

ExternalImageHandle ExternalImageRegistry::handleOf(tap::ResourceId texture) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_byTexture.find(texture);
    if (it == m_byTexture.end()) {
        return {};
    }
    return {it->second, m_slots[it->second - 1].generation};
}

std::vector<ExternalImageHandle> ExternalImageRegistry::findByImageHash(hash::Hash64 imageHash) const {
    std::vector<ExternalImageHandle> out;
    if (imageHash == hash::kEmptyHash) {
        return out;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::uint32_t i = 1; i <= m_slots.size(); ++i) {
        const Slot& slot = m_slots[i - 1];
        if (slot.live && slot.info.imageHash == imageHash) {
            out.push_back({i, slot.generation});
        }
    }
    return out;
}

void ExternalImageRegistry::setReleaseListener(ReleaseListener listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onRelease = std::move(listener);
}

std::size_t ExternalImageRegistry::liveCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_live;
}

std::size_t ExternalImageRegistry::capacity() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots.size();
}

} // namespace fuse::relight::capture::texture
