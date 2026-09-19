#include <fuse/editor/ai_tree_profile_picker.hpp>

namespace fuse::editor {

AiTreeProfilePicker::AiTreeProfilePicker(EditorHost& host) : m_host(host) {
    refreshOptions();
}

void AiTreeProfilePicker::refreshOptions() {
    m_options = {
        {0u, "move_toward (aiMovement.cs)", "aiMovement.cs"},
        {1u, "patrol_squad (aiBehaviors.cs)", "aiBehaviors.cs"},
    };
}

void AiTreeProfilePicker::postSelectProfile(u32 profileId) {
    m_selectedProfileId = profileId;
    ++m_postCount;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "ai.tree_profile_id";
    command.propertyValue = std::to_string(profileId);
    m_host.postFromUi(std::move(command));
}

} // namespace fuse::editor
