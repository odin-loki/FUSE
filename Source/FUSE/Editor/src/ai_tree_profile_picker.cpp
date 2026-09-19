#include <fuse/editor/ai_tree_profile_picker.hpp>

#include <fuse/ai/uaisk_cs_parser.hpp>
#include <fuse/ai/uaisk_template_hooks.hpp>

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

} // namespace fuse::editor
