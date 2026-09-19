#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/destroyComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Destroy-on-trigger leaf (GMK DestroyComponent without SimObject/Con::).
class DestroyComponent : public Component {
public:
    DestroyComponent();
    explicit DestroyComponent(std::string name, bool destroyOnTrigger = true);

    const char* typeName() const override { return "DestroyComponent"; }

    bool destroyOnTrigger() const { return m_destroyOnTrigger; }
    void setDestroyOnTrigger(bool destroyOnTrigger) { m_destroyOnTrigger = destroyOnTrigger; }

    bool triggered() const { return m_triggered; }
    u32 destroyCount() const { return m_destroyCount; }
    bool triggerDestroy();

private:
    bool m_destroyOnTrigger = true;
    bool m_triggered = false;
    u32 m_destroyCount = 0;
};

} // namespace fuse::mechanics
