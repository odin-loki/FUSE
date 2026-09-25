// FUSE Relight RL-4.1: external-image bindless registration (see bindless_images.hpp).
#include <fuse/relight/render/frame/bindless_images.hpp>

#include <algorithm>

namespace fuse::relight::render::frame {

namespace rr = fuse::renderer;

// ---- CpuBindlessHeap ---------------------------------------------------------------------------------------

rr::BindlessSlotHandle CpuBindlessHeap::registerTexture(const ExternalImageDesc&, std::uint64_t) {
    std::uint32_t index = 0;
    if (!m_free.empty()) {
        index = m_free.back();
        m_free.pop_back();
    } else if (m_slots.size() < rr::kMaxTextures) {
        index = static_cast<std::uint32_t>(m_slots.size());
        m_slots.emplace_back();
    } else {
        return rr::BindlessSlotHandle::invalid();
    }
    Slot& s = m_slots[index];
    s.occupied = true;
    s.retired = false;
    return rr::BindlessSlotHandle{rr::BindlessHeapKind::Texture, index, s.generation};
}

bool CpuBindlessHeap::validate(rr::BindlessSlotHandle slot) const {
    return slot.isValid() && slot.kind == rr::BindlessHeapKind::Texture && slot.index < m_slots.size() &&
           m_slots[slot.index].occupied && !m_slots[slot.index].retired &&
           m_slots[slot.index].generation == slot.generation;
}

bool CpuBindlessHeap::retire(rr::BindlessSlotHandle slot, std::uint64_t serial) {
    if (!validate(slot)) {
        return false;
    }
    Slot& s = m_slots[slot.index];
    s.retired = true;
    s.serial = serial;
    ++s.generation; // stale at once
    return true;
}

std::uint32_t CpuBindlessHeap::collect(std::uint64_t completedSerial) {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
        Slot& s = m_slots[i];
        if (s.occupied && s.retired && s.serial <= completedSerial) {
            s.occupied = false;
            s.retired = false;
            m_free.push_back(i);
            ++n;
        }
    }
    return n;
}

std::uint32_t CpuBindlessHeap::shaderHandle(rr::BindlessSlotHandle slot) const {
    return validate(slot) ? rr::packBindlessShaderHandle(rr::BindlessResourceType::SampledImage, slot.index,
                                                         slot.generation)
                          : rr::kBindlessInvalidShaderHandle;
}

// ---- BindlessImageRegistry ---------------------------------------------------------------------------------

BindlessImageRegistry::BindlessImageRegistry(IBindlessHeap& heap, IImageViewFactory* views)
    : m_heap(heap), m_views(views) {}

BindlessImageRegistry::~BindlessImageRegistry() { releaseAll(); }

rr::BindlessSlotHandle BindlessImageRegistry::registerImage(const ExternalImageDesc& image, std::uint64_t serial) {
    if (image.vkImage == 0 || image.texture == tap::kNoResource) {
        return rr::BindlessSlotHandle::invalid();
    }
    auto it = m_entries.find(image.texture);
    if (it != m_entries.end()) {
        if (it->second.vkImage == image.vkImage && m_heap.validate(it->second.slot)) {
            return it->second.slot;
        }
        retireEntry(it->second, serial); // DXVK recreated the texture's image
        m_entries.erase(it);
    }
    Entry e;
    e.vkImage = image.vkImage;
    e.owned = image.owned;
    if (m_views) {
        e.view = m_views->createView(image);
        if (e.view == 0) {
            ++m_stats.viewFailures;
        }
    }
    e.slot = m_heap.registerTexture(image, e.view);
    if (!e.slot.isValid()) {
        if (e.view != 0 && m_views) {
            m_views->destroyView(e.view);
        }
        ++m_stats.heapFull;
        return rr::BindlessSlotHandle::invalid();
    }
    m_entries.emplace(image.texture, e);
    ++m_stats.registered;
    ++m_stats.live;
    ++(image.owned ? m_stats.owned : m_stats.external);
    return e.slot;
}

void BindlessImageRegistry::retireEntry(const Entry& e, std::uint64_t serial) {
    if (m_heap.retire(e.slot, serial)) {
        m_retired.push_back(Retired{e.view, serial});
    } else if (e.view != 0 && m_views) {
        m_views->destroyView(e.view); // stale slot: nothing reads through it
    }
    --m_stats.live;
    --(e.owned ? m_stats.owned : m_stats.external);
    ++m_stats.released;
    m_stats.retired = static_cast<std::uint32_t>(m_retired.size());
}

bool BindlessImageRegistry::release(tap::ResourceId texture, std::uint64_t serial) {
    auto it = m_entries.find(texture);
    if (it == m_entries.end()) {
        return false;
    }
    retireEntry(it->second, serial);
    m_entries.erase(it);
    return true;
}

std::uint32_t BindlessImageRegistry::collect(std::uint64_t completedSerial) {
    const std::uint32_t reclaimed = m_heap.collect(completedSerial);
    auto keep = std::remove_if(m_retired.begin(), m_retired.end(), [&](const Retired& r) {
        if (r.serial > completedSerial) {
            return false;
        }
        if (r.view != 0 && m_views) {
            m_views->destroyView(r.view);
        }
        return true;
    });
    m_retired.erase(keep, m_retired.end());
    m_stats.reclaimed += reclaimed;
    m_stats.retired = static_cast<std::uint32_t>(m_retired.size());
    return reclaimed;
}

void BindlessImageRegistry::releaseAll() {
    while (!m_entries.empty()) {
        release(m_entries.begin()->first, 0);
    }
    collect(UINT64_MAX);
}

rr::BindlessSlotHandle BindlessImageRegistry::handleOf(tap::ResourceId texture) const {
    auto it = m_entries.find(texture);
    return it == m_entries.end() ? rr::BindlessSlotHandle::invalid() : it->second.slot;
}

std::uint32_t BindlessImageRegistry::shaderHandle(tap::ResourceId texture) const {
    auto it = m_entries.find(texture);
    return it == m_entries.end() ? 0u : m_heap.shaderHandle(it->second.slot);
}

std::uint64_t BindlessImageRegistry::viewOf(tap::ResourceId texture) const {
    auto it = m_entries.find(texture);
    return it == m_entries.end() ? 0u : it->second.view;
}

} // namespace fuse::relight::render::frame
