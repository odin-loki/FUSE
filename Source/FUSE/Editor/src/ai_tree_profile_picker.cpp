#include <fuse/editor/ai_tree_profile_picker.hpp>

#include <fuse/ai/uaisk_cs_parser.hpp>
#include <fuse/ai/uaisk_template_hooks.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>

namespace fuse::editor {

AiTreeProfilePicker::AiTreeProfilePicker(EditorHost& host) : m_host(host) {
    refreshOptions();
}

void AiTreeProfilePicker::refreshOptions() {
    m_options.clear();
    for (const fuse::ai::uaisk::TemplateHook& hook : fuse::ai::uaisk::kTemplateHooks) {
        const fuse::u32 profileId = fuse::ai::uaisk::profileIdForRegistryType(hook.fuseRegistryTypeId);
        const std::string label =
            std::string(hook.fuseRegistryTypeId) + " (" + std::string(hook.uaiskModule) + ")";
        m_options.push_back({profileId, label, std::string(hook.uaiskModule)});
    }
}

const AiTreeProfileOption* AiTreeProfilePicker::optionForProfile(u32 profileId) const {
    for (const AiTreeProfileOption& option : m_options) {
        if (option.profileId == profileId) {
            return &option;
        }
    }
    return nullptr;
}

void AiTreeProfilePicker::postSelectProfile(u32 profileId) {
    m_selectedProfileId = profileId;
    if (const AiTreeProfileOption* option = optionForProfile(profileId)) {
        m_selectedUaiskModule = option->uaiskModule;
    }
    ++m_postCount;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "ai.tree_profile_id";
    command.propertyValue = std::to_string(profileId);
    m_host.postFromUi(std::move(command));
}

void AiTreeProfilePicker::postSelectModule(std::string_view uaiskModule) {
    for (const AiTreeProfileOption& option : m_options) {
        if (option.uaiskModule == uaiskModule) {
            postSelectProfile(option.profileId);
            return;
        }
    }
}

void AiTreeProfilePicker::postSelectAgent(u32 agentIndex) {
    m_selectedAgentIndex = agentIndex;
    ++m_postCount;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "ai.selected_agent";
    command.propertyValue = std::to_string(agentIndex);
    m_host.postFromUi(std::move(command));
}

void AiTreeProfilePicker::postBindAgentEntity(u32 agentIndex, fuse::Handle<fuse::Object> entity) {
    m_selectedAgentIndex = agentIndex;
    ++m_postCount;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "ai.agent_entity";
    command.propertyValue =
        std::to_string(agentIndex) + ":" + std::to_string(entity.index()) + ":" + std::to_string(entity.generation());
    m_host.postFromUi(std::move(command));
}

void AiTreeProfilePicker::postBindSelectedEntity() {
    const fuse::ecs::EntityID selected = m_host.editorState().primarySelection;
    if (!selected.valid()) {
        return;
    }

    const fuse::Handle<fuse::Object> entity(selected.index, selected.generation);
    postBindAgentEntity(m_selectedAgentIndex, entity);
}

bool AiTreeProfilePicker::postBindSelectedEntityAndReloadTree(std::string_view watchPath, u32 profileId) {
    postBindSelectedEntity();
    return postTreeFileWatchReload(watchPath, profileId);
}

bool AiTreeProfilePicker::postTreeFileWatchReload(std::string_view watchPath, u32 profileId) {
    if (watchPath.empty()) {
        return false;
    }

    ++m_postCount;
    m_selectedProfileId = profileId;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "ai.tree_file_reload";
    command.propertyValue = "profile=" + std::to_string(profileId) + ";path=" + std::string(watchPath);
    m_host.postFromUi(std::move(command));
    return true;
}

bool AiTreeProfilePicker::postCodegenReload(std::string_view uaiskModule, std::string_view csText, u32 profileId) {
    if (uaiskModule.empty() || csText.empty()) {
        return false;
    }

    ++m_postCount;
    m_selectedUaiskModule = std::string(uaiskModule);
    m_selectedProfileId = profileId;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "ai.codegen_reload";
    command.propertyValue = "profile=" + std::to_string(profileId) + ";module=" + std::string(uaiskModule) + "\n" +
                            std::string(csText);
    m_host.postFromUi(std::move(command));
    return true;
}

} // namespace fuse::editor
