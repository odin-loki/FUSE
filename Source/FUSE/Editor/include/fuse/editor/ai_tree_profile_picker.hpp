#pragma once

#include <fuse/editor/editor_host.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

struct AiTreeProfileOption {
    u32 profileId = 0;
    std::string label;
    std::string uaiskModule;
};

/// WP-08 / U5 stub — posts `ai.tree_profile_id` through EditorHost (Qt-free).
class AiTreeProfilePicker {
public:
    explicit AiTreeProfilePicker(EditorHost& host);

    EditorHost& host() { return m_host; }
    const EditorHost& host() const { return m_host; }

    [[nodiscard]] const std::vector<AiTreeProfileOption>& options() const { return m_options; }
    [[nodiscard]] const AiTreeProfileOption* optionForProfile(u32 profileId) const;
    [[nodiscard]] u32 selectedProfileId() const { return m_selectedProfileId; }
    [[nodiscard]] const std::string& selectedUaiskModule() const { return m_selectedUaiskModule; }
    [[nodiscard]] u32 postCount() const { return m_postCount; }

    void refreshOptions();
    void postSelectProfile(u32 profileId);
    void postSelectModule(std::string_view uaiskModule);
    void postBindAgentEntity(u32 agentIndex, fuse::Handle<fuse::Object> entity);

private:
    EditorHost& m_host;
    std::vector<AiTreeProfileOption> m_options;
    u32 m_selectedProfileId = 0;
    std::string m_selectedUaiskModule;
    u32 m_postCount = 0;
};

} // namespace fuse::editor
