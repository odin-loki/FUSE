#include "editor_panels.hpp"

#include "property_pane_widget.hpp"

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace fuse::editor::qt {

namespace {

constexpr int kEntityIndexRole = Qt::UserRole + 1;
constexpr int kEntityGenerationRole = Qt::UserRole + 2;

ecs::EntityID itemEntity(const QTreeWidgetItem* item) {
    ecs::EntityID id{};
    if (item != nullptr) {
        id.index = item->data(0, kEntityIndexRole).toUInt();
        id.generation = item->data(0, kEntityGenerationRole).toUInt();
    }
    return id;
}

} // namespace

// ---- HierarchyWidget ------------------------------------------------------------------------------

HierarchyWidget::HierarchyWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search…"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("fuseHierarchyTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_tree, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this]() { refresh(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &HierarchyWidget::onItemSelectionChanged);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &HierarchyWidget::onContextMenuRequested);
}

QString HierarchyWidget::entityLabel(const ecs::Registry& registry, ecs::EntityID id) {
    const char* kind = "Entity";
    if (registry.has<ecs::SDFObject>(id)) {
        kind = "SDF Object";
    } else if (registry.has<ecs::Mesh>(id)) {
        kind = "Mesh";
    } else if (registry.has<ecs::PointLight>(id)) {
        kind = "Point Light";
    } else if (registry.has<ecs::DirectionalLight>(id)) {
        kind = "Directional Light";
    } else if (registry.has<ecs::SpotLight>(id)) {
        kind = "Spot Light";
    } else if (registry.has<ecs::Camera>(id)) {
        kind = "Camera";
    }
    return QStringLiteral("%1 #%2").arg(QString::fromLatin1(kind)).arg(id.index);
}

int HierarchyWidget::entityRowCount() const {
    int count = 0;
    for (QTreeWidgetItemIterator it(m_tree); *it != nullptr; ++it) {
        ++count;
    }
    return count;
}

void HierarchyWidget::refresh() {
    struct Row {
        ecs::EntityID id;
        ecs::EntityID parent;
        QString label;
    };
    std::vector<Row> rows;
    std::vector<ecs::EntityID> selected;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
            rows.push_back({id, t.parent, entityLabel(registry, id)});
        });
        selected = m_host.editorState().selectedEntities;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id.index < b.id.index; });

    const QString filter = m_search->text().trimmed();
    m_syncing = true;
    m_tree->clear();
    std::map<u32, QTreeWidgetItem*> items;
    auto isSelected = [&](ecs::EntityID id) {
        return std::find(selected.begin(), selected.end(), id) != selected.end();
    };
    // Flat when filtering (matches are shown without their non-matching ancestors).
    for (const Row& row : rows) {
        if (!filter.isEmpty() && !row.label.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        auto* item = new QTreeWidgetItem(QStringList{row.label});
        item->setData(0, kEntityIndexRole, row.id.index);
        item->setData(0, kEntityGenerationRole, row.id.generation);
        items[row.id.index] = item;
    }
    for (const Row& row : rows) {
        auto found = items.find(row.id.index);
        if (found == items.end()) {
            continue;
        }
        auto parentIt = filter.isEmpty() && row.parent.valid() ? items.find(row.parent.index) : items.end();
        if (parentIt != items.end() && parentIt->second != found->second) {
            parentIt->second->addChild(found->second);
        } else {
            m_tree->addTopLevelItem(found->second);
        }
    }
    m_tree->expandAll();
    for (const auto& [index, item] : items) {
        (void)index;
        item->setSelected(isSelected(itemEntity(item)));
    }
    m_syncing = false;
}

void HierarchyWidget::onItemSelectionChanged() {
    if (m_syncing) {
        return;
    }
    std::vector<ecs::EntityID> selection;
    for (QTreeWidgetItem* item : m_tree->selectedItems()) {
        selection.push_back(itemEntity(item));
    }
    const ecs::EntityID current = itemEntity(m_tree->currentItem());
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        EditorState& state = m_host.editorState();
        state.selectedEntities = selection;
        state.primarySelection = selection.empty() ? ecs::EntityID::null()
                                 : std::find(selection.begin(), selection.end(), current) != selection.end()
                                     ? current
                                     : selection.front();
    }
    emit selectionChanged();
}

void HierarchyWidget::onContextMenuRequested(const QPoint& pos) {
    const ecs::EntityID clicked = itemEntity(m_tree->itemAt(pos));
    QWidget* top = window();
    const QPoint inWindow = m_tree->viewport()->mapTo(top, pos);
    ContextMenuHostGeometry geometry{};
    geometry.devicePixelRatio = static_cast<f32>(devicePixelRatioF());
    geometry.windowWidth = static_cast<f32>(top->width());
    geometry.windowHeight = static_cast<f32>(top->height());
    m_contextMenu.setHostGeometry(geometry);
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::vec3 spawn{};
        if (clicked.valid()) {
            if (const ecs::Transform* t = m_host.editorScene().registry().get<ecs::Transform>(clicked)) {
                spawn = t->position;
            }
        }
        m_contextMenu.openAt(clicked, spawn, m_host.editorState(), static_cast<f32>(inWindow.x()),
                             static_cast<f32>(inWindow.y()));
    }
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    for (const ContextMenuItem& item : m_contextMenu.items()) {
        if (item.separatorBefore) {
            menu->addSeparator();
        }
        QAction* action = menu->addAction(QString::fromUtf8(item.label));
        action->setEnabled(item.enabled);
        const ContextMenuAction id = item.action;
        connect(action, &QAction::triggered, this, [this, id]() {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(m_sceneMutex);
                changed = m_contextMenu.activate(id, m_host.editorScene(), m_host.editorState(), m_host.undoStack());
            }
            if (changed) {
                emit sceneEdited();
            }
        });
    }
    menu->ensurePolished();
    const QSize hint = menu->sizeHint();
    m_contextMenu.setMenuSize(static_cast<f32>(hint.width()), static_cast<f32>(hint.height()));
    const ContextMenuPlacement& p = m_contextMenu.placement();
    menu->popup(top->mapToGlobal(QPoint(static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)))));
}

// ---- InspectorWidget ------------------------------------------------------------------------------

InspectorWidget::InspectorWidget(FeaturePaneBridge& bridge, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_bridge(bridge), m_sceneMutex(sceneMutex) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_pane = new PropertyPaneWidget(bridge, this);
    layout->addWidget(m_pane);
    m_sections = new QTreeWidget(this);
    m_sections->setObjectName(QStringLiteral("fuseInspectorSections"));
    m_sections->setColumnCount(2);
    m_sections->setHeaderLabels({tr("Property"), tr("Value")});
    m_sections->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    layout->addWidget(m_sections, 1);
}

void InspectorWidget::refresh() {
    std::vector<std::string> signature;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_pane->refresh();
        EditorHost& host = m_bridge.host();
        m_ecsInspector.sync(host.editorState(), host.editorScene());
        if (m_ecsInspector.hasSelection()) {
            signature.push_back(std::to_string(m_ecsInspector.target().index) + ":" +
                                std::to_string(m_ecsInspector.target().generation));
            for (const PropertyInspector::ComponentSection& section : m_ecsInspector.sections()) {
                signature.push_back("#" + section.componentName);
                for (const PropertyInspector::Field& field : section.fields) {
                    signature.push_back(field.name + "=" + field.value);
                }
            }
        }
    }
    if (signature == m_lastSignature) {
        return; // unchanged: keep the tree (and its expansion / scroll state) as is
    }
    m_lastSignature = signature;
    m_sections->clear();
    QTreeWidgetItem* section = nullptr;
    for (usize i = 1; i < signature.size(); ++i) {
        const std::string& entry = signature[i];
        if (entry.starts_with('#')) {
            section = new QTreeWidgetItem(m_sections, QStringList{QString::fromStdString(entry.substr(1))});
            section->setFirstColumnSpanned(true);
            continue;
        }
        const usize eq = entry.find('=');
        auto* row = new QTreeWidgetItem(QStringList{QString::fromStdString(entry.substr(0, eq)),
                                                    QString::fromStdString(entry.substr(eq + 1))});
        if (section != nullptr) {
            section->addChild(row);
        } else {
            m_sections->addTopLevelItem(row);
        }
    }
    m_sections->expandAll();
}

// ---- ConsoleWidget --------------------------------------------------------------------------------

ConsoleWidget::ConsoleWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("fuseConsoleLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    layout->addWidget(m_log, 1);
    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("fuseConsoleInput"));
    m_input->setPlaceholderText(tr("Command (help)"));
    layout->addWidget(m_input);
    connect(m_input, &QLineEdit::returnPressed, this, &ConsoleWidget::onSubmit);
    fuse::log::Logger::instance().setSink(&ConsoleWidget::logSink, this);
    m_panel.addLog(fuse::log::Level::Info, "FUSE editor console ready");
    rerender();
}

ConsoleWidget::~ConsoleWidget() {
    fuse::log::Logger::instance().setSink(nullptr, nullptr);
}

void ConsoleWidget::logSink(fuse::log::Level level, const char* message, void* userData) {
    auto* self = static_cast<ConsoleWidget*>(userData);
    std::fprintf(stderr, "%s\n", message);
    std::lock_guard<std::mutex> lock(self->m_queueMutex);
    if (self->m_queue.size() < 4096u) {
        self->m_queue.emplace_back(level, message);
    }
}

void ConsoleWidget::drainLog() {
    std::vector<std::pair<fuse::log::Level, std::string>> pending;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        pending.swap(m_queue);
    }
    if (pending.empty()) {
        return;
    }
    for (const auto& [level, text] : pending) {
        m_panel.addLog(level, text.c_str());
    }
    rerender();
}

void ConsoleWidget::rerender() {
    const std::vector<ConsolePanel::LogLine> lines = m_panel.filteredLines();
    if (lines.size() < m_renderedLines) {
        m_log->clear();
        m_renderedLines = 0;
    }
    // Repeated lines collapse in the panel (repeatCount): re-render the tail line in that case.
    if (m_renderedLines > 0 && m_renderedLines == lines.size()) {
        return;
    }
    for (usize i = m_renderedLines; i < lines.size(); ++i) {
        const ConsolePanel::LogLine& line = lines[i];
        QString text = QStringLiteral("[%1] %2").arg(QString::fromLatin1(ConsolePanel::levelLabel(line.level)),
                                                    QString::fromStdString(line.text));
        if (line.repeatCount > 1) {
            text += QStringLiteral(" (x%1)").arg(line.repeatCount);
        }
        m_log->appendPlainText(text);
    }
    m_renderedLines = lines.size();
}

void ConsoleWidget::onSubmit() {
    const QByteArray line = m_input->text().toUtf8();
    m_input->clear();
    m_panel.executeCommand(line.constData());
    m_log->clear();
    m_renderedLines = 0;
    rerender();
}

// ---- AssetBrowserWidget ---------------------------------------------------------------------------

AssetBrowserWidget::AssetBrowserWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter assets…"));
    layout->addWidget(m_filter);
    m_list = new QListWidget(this);
    layout->addWidget(m_list, 1);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_model.setSearchFilter(text.toUtf8().constData());
        rebuild();
    });
}

void AssetBrowserWidget::setProjectRoot(const QString& root) {
    m_model.init(root.toUtf8().constData());
    m_model.refresh();
    rebuild();
}

void AssetBrowserWidget::rebuild() {
    m_list->clear();
    for (const AssetBrowser::AssetEntry& entry : m_model.entries()) { // already filtered by the model
        m_list->addItem(QStringLiteral("%1  (%2)").arg(QString::fromStdString(entry.name),
                                                      QString::fromLatin1(AssetBrowser::assetTypeLabel(entry.type))));
    }
}

// ---- ProfilerWidget -------------------------------------------------------------------------------

ProfilerWidget::ProfilerWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("fuseProfilerSummary"));
    layout->addWidget(m_summary);
    layout->addStretch(1);
    refresh();
}

void ProfilerWidget::pushFrame(const ProfilerPanel::FrameProfileData& data) {
    m_panel.pushFrameData(data);
}

void ProfilerWidget::refresh() {
    const ProfilerPanel::FrameProfileData& f = m_panel.latestFrame();
    m_summary->setText(tr("Frames: %1\nCPU: %2 ms\nEditor UI: %3 ms\nTarget: %4 fps")
                           .arg(m_panel.frameCount())
                           .arg(f.cpuMs, 0, 'f', 3)
                           .arg(f.postprocessMs, 0, 'f', 3)
                           .arg(m_panel.targetFps(), 0, 'f', 0));
}

// ---- MaterialEditorWidget -------------------------------------------------------------------------

MaterialEditorWidget::MaterialEditorWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex) {
    auto* layout = new QFormLayout(this);
    m_summary = new QLabel(this);
    layout->addRow(m_summary);
    m_roughness = new QDoubleSpinBox(this);
    m_metallic = new QDoubleSpinBox(this);
    for (QDoubleSpinBox* spin : {m_roughness, m_metallic}) {
        spin->setRange(0.0, 1.0);
        spin->setSingleStep(0.05);
        spin->setDecimals(3);
    }
    layout->addRow(tr("Roughness"), m_roughness);
    layout->addRow(tr("Metallic"), m_metallic);
    connect(m_roughness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_panel.setRoughness(static_cast<f32>(v), m_host.commandStack());
    });
    connect(m_metallic, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_panel.setMetallic(static_cast<f32>(v), m_host.commandStack());
    });
    refresh();
}

void MaterialEditorWidget::refresh() {
    bool has = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_panel.sync(m_host.editorState(), m_panel.catalogCount());
        has = m_panel.hasSelectedMaterial();
    }
    m_summary->setText(has ? tr("Material %1").arg(m_panel.selectedMaterialId()) : tr("No material selected"));
    m_roughness->setEnabled(has);
    m_metallic->setEnabled(has);
}

// ---- SdfSculptWidget ------------------------------------------------------------------------------

SdfSculptWidget::SdfSculptWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex) {
    auto* layout = new QFormLayout(this);
    m_op = new QComboBox(this);
    m_op->addItems({tr("Add"), tr("Subtract"), tr("Smooth"), tr("Roughen"), tr("Paint")});
    m_radius = new QDoubleSpinBox(this);
    m_radius->setRange(0.01, 100.0);
    m_radius->setValue(m_panel.brush().radius);
    m_alpha = new QDoubleSpinBox(this);
    m_alpha->setRange(0.0, 1.0);
    m_alpha->setDecimals(3);
    m_alpha->setValue(m_panel.brush().blendAlpha);
    m_summary = new QLabel(this);
    layout->addRow(tr("Brush"), m_op);
    layout->addRow(tr("Radius"), m_radius);
    layout->addRow(tr("GRIA α"), m_alpha);
    layout->addRow(m_summary);
    connect(m_op, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int i) { m_panel.setBrushOperation(static_cast<SdfSculptPanel::BrushOp>(i)); });
    connect(m_radius, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { m_panel.setBrushRadius(static_cast<f32>(v)); });
    connect(m_alpha, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { m_panel.setBlendAlpha(static_cast<f32>(v)); });
    refresh();
}

void SdfSculptWidget::refresh() {
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_panel.sync(m_host.editorState(), m_host.editorScene());
    }
    m_summary->setText(tr("Strokes: %1 | Sculpt %2")
                           .arg(m_panel.strokeCount())
                           .arg(m_panel.sculptActive() ? tr("active") : tr("idle")));
}

} // namespace fuse::editor::qt
