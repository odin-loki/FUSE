#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/soundComponent.h

#include <fuse/mechanics/component.hpp>

#include <string>

namespace fuse::mechanics {

/// Sound playback leaf (GMK SoundComponent without SimObject/Con::).
class SoundComponent : public Component {
public:
    SoundComponent();
    explicit SoundComponent(std::string name, std::string assetId);

    const char* typeName() const override { return "SoundComponent"; }

    const std::string& assetId() const { return m_assetId; }
    void setAssetId(std::string assetId) { m_assetId = std::move(assetId); }

    u32 playCount() const { return m_playCount; }
    bool playing() const { return m_playing; }

    void play();
    void stop();

private:
    std::string m_assetId;
    bool m_playing = false;
    u32 m_playCount = 0;
};

} // namespace fuse::mechanics
