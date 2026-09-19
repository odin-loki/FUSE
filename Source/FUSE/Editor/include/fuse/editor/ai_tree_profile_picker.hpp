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
    [[nodiscard]] u32 selectedProfileId() const { return m_selectedProfileId; }
    [[nodiscard]] u32 postCount() const { return m_postCount; }

    void refreshOptions();
    void postSelectProfile(u32 profileId);

private:
    EditorHost& m_host;
    std::vector<AiTreeProfileOption> m_options;
    u32 m_selectedProfileId = 0;
    u32 m_postCount = 0;
};

} // namespace fuse::editor
