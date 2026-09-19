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
    [[nodiscard]] u32 selectedAgentIndex() const { return m_selectedAgentIndex; }
    [[nodiscard]] u32 postCount() const { return m_postCount; }

    void refreshOptions();
    void postSelectProfile(u32 profileId);
    void postSelectModule(std::string_view uaiskModule);
    void postSelectAgent(u32 agentIndex);
    void postBindAgentEntity(u32 agentIndex, fuse::Handle<fuse::Object> entity);
    /// Bind the currently selected agent to the editor's primary entity selection.
    void postBindSelectedEntity();
    /// Bind selected entity and hot-reload the watched UAISK tree profile from disk.
    bool postBindSelectedEntityAndReloadTree(std::string_view watchPath, u32 profileId);
    /// Codegen UAISK `.cs` and hot-reload the mapped profile on the game thread.
    bool postCodegenReload(std::string_view uaiskModule, std::string_view csText, u32 profileId);
    /// Register a disk-backed tree watch and reload on the game thread.
    bool postTreeFileWatchReload(std::string_view watchPath, u32 profileId);
    /// Hot-reload via inotify/FSEvents poll path (picker → game-thread inotify poll).
    bool postInotifyTreeHotReload(std::string_view watchPath, u32 profileId);

private:
    EditorHost& m_host;
    std::vector<AiTreeProfileOption> m_options;
    u32 m_selectedProfileId = 0;
    std::string m_selectedUaiskModule;
    u32 m_selectedAgentIndex = 0;
    u32 m_postCount = 0;
};

} // namespace fuse::editor
