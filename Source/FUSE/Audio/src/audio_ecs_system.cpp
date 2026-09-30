#include <fuse/audio/audio_ecs_system.hpp>

#include <fuse/audio/audio_engine.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>

namespace fuse::audio {

namespace {

Vec3 translation_of(const ecs::Transform& transform) {
    const ecs::mat4& m = transform.local_to_world;
    return {m.data[12], m.data[13], m.data[14]};
}

} // namespace

bool AudioEcsSystem::init(AudioEngine& engine, usize expected_sources) {
    shutdown();
    m_engine = &engine;
    m_tracked.reserve(expected_sources);
    m_index.reserve(expected_sources);
    m_listenerVoice = m_voices.create_entity();
    return engine.is_initialized();
}

void AudioEcsSystem::shutdown() {
    for (const Tracked& tracked : m_tracked) {
        m_voices.destroy_entity(tracked.voice);
    }
    m_tracked.clear();
    m_index.clear();
    if (m_listenerVoice != kInvalidEntity) {
        m_voices.destroy_entity(m_listenerVoice);
        m_listenerVoice = kInvalidEntity;
    }
    m_engine = nullptr;
    m_stamp = 0;
    m_stats = {};
}

EntityId AudioEcsSystem::voice_of(ecs::EntityID entity) const {
    const auto it = m_index.find(key_of(entity));
    return it == m_index.end() ? kInvalidEntity : m_tracked[it->second].voice;
}

void AudioEcsSystem::update(ecs::Registry& registry, f32 dt) {
    if (m_engine == nullptr) {
        return;
    }
    ++m_stamp;
    const u32 stamp = m_stamp;

    // 1 + 2: reconcile sources and push game-side state.
    u32 active = 0;
    registry.each<AudioSource>([&](ecs::EntityID entity, AudioSource& component) {
        const u64 key = key_of(entity);
        auto it = m_index.find(key);
        if (it == m_index.end()) {
            Tracked tracked;
            tracked.entity = entity;
            tracked.voice = m_voices.create_entity();
            m_voices.add_source(tracked.voice, component.desc);
            if (component.desc.play_on_awake) {
                component.playing = true;
            }
            it = m_index.emplace(key, static_cast<u32>(m_tracked.size())).first;
            m_tracked.push_back(tracked);
            ++m_stats.voices_created;
        }
        Tracked& tracked = m_tracked[it->second];
        tracked.stamp = stamp;
        AudioSource* voice = m_voices.find_source(tracked.voice);
        if (voice == nullptr) {
            return;
        }
        const ecs::Transform* transform = registry.get<ecs::Transform>(entity);
        const Vec3 position = transform != nullptr ? translation_of(*transform) : component.position;
        component.position = position;
        voice->desc = component.desc;
        voice->playing = component.playing;
        voice->paused = component.paused;
        voice->position = position;
        m_voices.set_position(tracked.voice, position);
        if (component.playing && !component.paused) {
            ++active;
        }
    });

    // Release voices whose component / entity is gone (swap-remove keeps the table dense).
    for (u32 i = 0; i < static_cast<u32>(m_tracked.size());) {
        if (m_tracked[i].stamp == stamp) {
            ++i;
            continue;
        }
        m_voices.destroy_entity(m_tracked[i].voice);
        m_index.erase(key_of(m_tracked[i].entity));
        ++m_stats.voices_released;
        const u32 last = static_cast<u32>(m_tracked.size()) - 1u;
        if (i != last) {
            m_tracked[i] = m_tracked[last];
            m_index[key_of(m_tracked[i].entity)] = i;
        }
        m_tracked.pop_back();
    }

    // Listener: the first AudioListener in registry order.
    bool listenerFound = false;
    registry.each<AudioListener>([&](ecs::EntityID entity, AudioListener& component) {
        if (listenerFound) {
            return;
        }
        listenerFound = true;
        if (const ecs::Transform* transform = registry.get<ecs::Transform>(entity)) {
            const ecs::mat4& m = transform->local_to_world;
            component.position = translation_of(*transform);
            const Vec3 forward{-m.data[8], -m.data[9], -m.data[10]};
            const Vec3 up{m.data[4], m.data[5], m.data[6]};
            if (forward.length() > 1e-6f && up.length() > 1e-6f) {
                component.forward = forward.normalized();
                component.up = up.normalized();
            }
        }
        AudioListener* listener = m_voices.listener();
        if (listener == nullptr) {
            listener = m_voices.set_listener(m_listenerVoice);
        }
        *listener = component;
        m_voices.set_position(m_listenerVoice, component.position);
    });
    if (!listenerFound && m_voices.listener() != nullptr) {
        // The listener component went away: drop the engine listener (structural, rare).
        m_voices.destroy_entity(m_listenerVoice);
        m_listenerVoice = m_voices.create_entity();
    }
    m_stats.has_listener = listenerFound;
    m_stats.active_sources = active;

    // 3: mix.
    m_engine->update(m_voices, dt);

    // 4: engine-side state back into the components.
    for (const Tracked& tracked : m_tracked) {
        const AudioSource* voice = m_voices.find_source(tracked.voice);
        AudioSource* component = registry.get<AudioSource>(tracked.entity);
        if (voice == nullptr || component == nullptr) {
            continue;
        }
        component->play_head = voice->play_head;
        component->playing = voice->playing;
        component->backend_source = voice->backend_source;
    }
}

} // namespace fuse::audio
