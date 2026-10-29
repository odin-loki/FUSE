#pragma once
// UI9: the FUSE UI pages of the seven panels on EditorPanels (Project Hub, Asset Browser, Material Editor, SDF Sculpt,
// Profiler, Sequencer, Timeline). Widget ids mirror the Qt objectNames (fuseMaterialNew -> material.new, ...).
#include "fuse/editor/fui/editor_shell.hpp"

#include <fuse/editor/editor_panel_models.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <chrono>

namespace fuse::editor::fui {

class ShellPanelPages {
public:
    ShellPanelPages(EditorShellWindow& shell, EditorPanels& models);
    /// Builds `panelId`'s page; false for ids this class does not own.
    bool build(std::string_view panelId, NodeHandle page);
    /// Pulls the models into the widgets (cheap when nothing changed). `uiMs` is the shell's last sync time; `frame`
    /// marks the per-frame call (profiler sample + timeline playback happen there only).
    void sync(f32 uiMs, bool frame);
    /// Capture-phase events from the shell root: asset context menu (right click), timeline keys ([ ] Home End L F).
    bool event(const fuse::ui::Event& e, NodeHandle target);
    /// Asset context menu actions (Qt AssetBrowserWidget): 0 Open, 1 Copy Path, 2 Copy Relative Path, 3 Rename...,
    /// 4 New Folder..., 5 Refresh. Rename / New Folder open the inline name row (assets.name / assets.name_ok).
    void assetsMenuAction(u32 action);
    static constexpr const char* kAssetMenu = "Open\nCopy Path\nCopy Relative Path\nRename...\n-\nNew Folder...\nRefresh";

private:
    void buildHub_(NodeHandle page);
    void buildAssets_(NodeHandle page);
    void buildMaterial_(NodeHandle page);
    void buildSculpt_(NodeHandle page);
    void buildProfiler_(NodeHandle page);
    void buildSequencer_(NodeHandle page);
    void buildTimeline_(NodeHandle page);
    void buildNavigation_(NodeHandle page);
    void syncNavigation_();
    void buildTerrain_(NodeHandle page);
    void syncTerrain_();
    NodeHandle m_trCells{}, m_trSize{}, m_trHeight{}, m_trSeed{}, m_trMode{}, m_trRadius{}, m_trStrength{}, m_trSculpt{}, m_trStatus{};
    std::string m_trKey;
    void syncHub_();
    void syncAssets_();
    void syncMaterial_();
    void syncSculpt_();
    void syncProfiler_();
    void syncSequencer_();
    void syncTimeline_();
    NodeHandle row_(NodeHandle parent);
    NodeHandle field_(NodeHandle parent, std::string_view label);

    EditorShellWindow& m_shell;
    fuse::ui::UiContext& m_ui;
    EditorPanels& m_m;
    std::chrono::steady_clock::time_point m_lastTick{};
    u64 m_seenRevision = 0, m_seenView = ~0ull;

    NodeHandle m_hubList, m_hubName, m_hubInfo, m_hubRoot;
    std::vector<std::string> m_hubProjects;
    std::string m_hubKey;

    NodeHandle m_assetFilter, m_assetPath, m_assetList, m_assetInfo;
    NodeHandle m_assetGrid{}, m_assetGridToggle{}, m_assetType{}, m_assetCrumbs{}, m_assetNameRow{}, m_assetName{};
    std::vector<std::string> m_assetPaths, m_assetNames, m_assetTips;
    fuse::ui::widgets::ItemModel m_assetModel{};
    std::string m_assetRenaming; ///< path being renamed ("" = New Folder) while the name row is open
    i32 selectedAsset_() const;
    bool within_(NodeHandle n, NodeHandle ancestor) const;
    std::string m_assetKey;

    NodeHandle m_matList, m_matPreview, m_matColor, m_matRough, m_matMetal, m_matShader, m_matTexture, m_matUv, m_matAssign, m_matSummary;
    fuse::ui::Image m_previewImg;
    u32 m_previewIndex = 0;
    std::string m_matKey;

    NodeHandle m_scMode, m_scOp, m_scShape, m_scMaterial, m_scRadius, m_scStrength, m_scSpacing, m_scAlpha, m_scSym, m_scSummary;
    std::string m_scKey;

    NodeHandle m_prSummary, m_prChart, m_prPause;
    u32 m_prSeen = ~0u;

    NodeHandle m_sqLink, m_sqScrub, m_sqPreview;
    std::string m_sqKey;

    NodeHandle m_navCell, m_navExtent, m_navShow, m_navFrom[2], m_navTo[2], m_navStatus;
    std::string m_navKey;

    NodeHandle m_tlBox, m_tlWidget, m_tlPlay, m_tlTime, m_tlLength, m_tlStatus;
    NodeHandle m_tlLoop{}, m_tlSnap{};
    std::string m_tlKey;
};

} // namespace fuse::editor::fui
