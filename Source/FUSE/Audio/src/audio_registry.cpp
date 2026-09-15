#include <fuse/audio/audio_registry.hpp>

#include <algorithm>

namespace fuse::audio {

EntityId AudioRegistry::create_entity() {
    const EntityId id = m_nextEntity++;
    m_positions[id] = Vec3{};
    return id;
}

void AudioRegistry::destroy_entity(EntityId id) {
    m_sources.erase(id);
    m_positions.erase(id);
    m_sourceOrder.erase(std::remove(m_sourceOrder.begin(), m_sourceOrder.end(), id),
                        m_sourceOrder.end());
    if (m_listenerEntity.has_value() && *m_listenerEntity == id) {
        m_listenerEntity.reset();
        m_listener.reset();
    }
}

AudioSource* AudioRegistry::add_source(EntityId id, const AudioSourceDesc& desc) {
    AudioSource source;
    source.desc = desc;
    source.position = position(id);
    if (desc.play_on_awake) {
        source.playing = true;
    }
    m_sources[id] = source;
    if (std::find(m_sourceOrder.begin(), m_sourceOrder.end(), id) == m_sourceOrder.end()) {
        m_sourceOrder.push_back(id);
    }
    return &m_sources[id];
}

AudioListener* AudioRegistry::set_listener(EntityId id) {
    m_listenerEntity = id;
    AudioListener listener;
    listener.position = position(id);
    m_listener = listener;
    return &(*m_listener);
}

AudioSource* AudioRegistry::find_source(EntityId id) {
    const auto it = m_sources.find(id);
    return it == m_sources.end() ? nullptr : &it->second;
}

const AudioSource* AudioRegistry::find_source(EntityId id) const {
    const auto it = m_sources.find(id);
    return it == m_sources.end() ? nullptr : &it->second;
}

AudioListener* AudioRegistry::listener() {
    return m_listener.has_value() ? &(*m_listener) : nullptr;
}

const AudioListener* AudioRegistry::listener() const {
    return m_listener.has_value() ? &(*m_listener) : nullptr;
}

void AudioRegistry::set_position(EntityId id, const Vec3& position) {
    m_positions[id] = position;
    if (AudioSource* source = find_source(id)) {
        source->position = position;
    }
    if (m_listenerEntity.has_value() && *m_listenerEntity == id && m_listener.has_value()) {
        m_listener->position = position;
    }
}

Vec3 AudioRegistry::position(EntityId id) const {
    const auto it = m_positions.find(id);
    return it == m_positions.end() ? Vec3{} : it->second;
}

} // namespace fuse::audio
