#pragma once

// Ore: third_party/addons/3DAAK (ShapeBase examine / lore interaction scripts)

#include <fuse/adventure/interactable.hpp>

#include <string>

namespace fuse::adventure {

/// Lore-bearing world object (3DAAK examine / `onUse` without item consumption).
class ExamineInteractable : public IInteractable {
public:
    explicit ExamineInteractable(std::string description);

    const std::string& description() const { return m_description; }
    u32 examineCount() const { return m_examineCount; }
    const std::string& lastExaminedBy() const { return m_lastExaminedBy; }

    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

    InteractResult onExamine(InteractContext& ctx);

private:
    std::string m_description;
    u32 m_examineCount = 0;
    std::string m_lastExaminedBy;
};

} // namespace fuse::adventure
