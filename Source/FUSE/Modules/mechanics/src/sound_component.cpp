#include <fuse/mechanics/sound_component.hpp>

namespace fuse::mechanics {

SoundComponent::SoundComponent() : Component("SoundComponent") {}

SoundComponent::SoundComponent(std::string name, std::string assetId)
    : Component(std::move(name)), m_assetId(std::move(assetId)) {}

void SoundComponent::play() {
    m_playing = true;
    ++m_playCount;
}

void SoundComponent::stop() {
    m_playing = false;
}

} // namespace fuse::mechanics
