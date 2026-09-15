#pragma once

#include <fuse/audio/audio_components.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

namespace fuse::audio {

/// Minimal ECS facade for audio components (Track A B3 stub until full ECS lands).
class AudioRegistry {
public:
    EntityId create_entity();
    void destroy_entity(EntityId id);

    AudioSource* add_source(EntityId id, const AudioSourceDesc& desc = {});
    AudioListener* set_listener(EntityId id);

    AudioSource* find_source(EntityId id);
    const AudioSource* find_source(EntityId id) const;

    AudioListener* listener();
    const AudioListener* listener() const;

    const std::vector<EntityId>& source_entities() const { return m_sourceOrder; }

    void set_position(EntityId id, const Vec3& position);
    Vec3 position(EntityId id) const;

private:
    EntityId m_nextEntity = 1;
    std::unordered_map<EntityId, AudioSource> m_sources;
    std::vector<EntityId> m_sourceOrder;
    std::unordered_map<EntityId, Vec3> m_positions;
    std::optional<EntityId> m_listenerEntity;
    std::optional<AudioListener> m_listener;
};

} // namespace fuse::audio
