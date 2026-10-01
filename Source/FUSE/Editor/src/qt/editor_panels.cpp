#include "editor_panels.hpp"

#include "property_pane_widget.hpp"

#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/component_schema.hpp>
#include <fuse/editor/editor_console_commands.hpp>

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QGroupBox>
#include <QScrollArea>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QKeyEvent>
#include <QPalette>
#include <QSignalBlocker>
#include <QToolButton>
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

Handle<Object> entityHandle(ecs::EntityID id) {
    return id.valid() ? Handle<Object>(id.index, id.generation) : Handle<Object>::invalid();
}

} // namespace

// ---- HierarchyTreeWidget --------------------------------------------------------------------------

HierarchyTreeWidget::HierarchyTreeWidget(QWidget* parent) : QTreeWidget(parent) {
    setDragEnabled(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
}

QTreeWidgetItem* HierarchyTreeWidget::dropParentAt(const QPoint& pos) const {
    QTreeWidgetItem* target = itemAt(pos);
    if (target == nullptr) {
        return nullptr; // empty area: scene root
    }
    const QRect rect = visualItemRect(target);
    const int margin = rect.height() / 4;
    if (pos.y() < rect.top() + margin || pos.y() > rect.bottom() - margin) {
        return target->parent(); // between rows: sibling of the target
    }
    return target;
}

bool HierarchyTreeWidget::acceptsDrag_(const QDropEvent* event) const {
    // Internal moves only: our own rows (a synthetic drag without QDrag in flight has no source).
    return (event->source() == this || event->source() == nullptr) && event->mimeData() != nullptr &&
           event->mimeData()->hasFormat(QStringLiteral("application/x-qabstractitemmodeldatalist"));
}

void HierarchyTreeWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (!acceptsDrag_(event)) {
        event->ignore();
        return;
    }
    QTreeWidget::dragEnterEvent(event); // drop indicator / auto-scroll state
    event->acceptProposedAction();
}

void HierarchyTreeWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (!acceptsDrag_(event)) {
        event->ignore();
        return;
    }
    QTreeWidget::dragMoveEvent(event);
    event->acceptProposedAction();
}

void HierarchyTreeWidget::dropEvent(QDropEvent* event) {
    if (!acceptsDrag_(event)) {
        event->ignore();
        return;
    }
    const QList<QTreeWidgetItem*> dragged = selectedItems();
    QTreeWidgetItem* newParent = dropParentAt(event->position().toPoint());
    // The registry is the source of truth: report the drop, never move the Qt rows here (a copy
    // action keeps QAbstractItemView from deleting the dragged rows after the drag returns).
    event->setDropAction(Qt::CopyAction);
    event->accept();
    if (!dragged.isEmpty()) {
        emit rowsDropped(dragged, newParent);
    }
}

// ---- HierarchyWidget ------------------------------------------------------------------------------

HierarchyWidget::HierarchyWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search…"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_tree = new HierarchyTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("fuseHierarchyTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_tree, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this]() { refresh(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &HierarchyWidget::onItemSelectionChanged);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &HierarchyWidget::onContextMenuRequested);
    connect(m_tree, &HierarchyTreeWidget::rowsDropped, this, &HierarchyWidget::onRowsDropped);
}

QTreeWidgetItem* HierarchyWidget::itemFor(ecs::EntityID id) const {
    for (QTreeWidgetItemIterator it(m_tree); *it != nullptr; ++it) {
        if (itemEntity(*it) == id) {
            return *it;
        }
    }
    return nullptr;
}

int HierarchyWidget::reparentEntities(const std::vector<ecs::EntityID>& entities, ecs::EntityID newParent) {
    std::vector<ecs::EntityID> moves;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        const ecs::Registry& registry = m_host.editorScene().registry();
        const auto isMoved = [&](ecs::EntityID id) {
            return std::find(entities.begin(), entities.end(), id) != entities.end();
        };
        for (const ecs::EntityID id : entities) {
            if (!id.valid() || !registry.alive(id) || id == newParent) {
                continue;
            }
            // Keep a moved child under its moved ancestor.
            bool ancestorMoved = false;
            const ecs::Transform* t = registry.get<ecs::Transform>(id);
            for (u32 depth = 0; t != nullptr && t->parent.valid() && depth < 1024u; ++depth) {
                if (isMoved(t->parent)) {
                    ancestorMoved = true;
                    break;
                }
                t = registry.get<ecs::Transform>(t->parent);
            }
            const ecs::Transform* own = registry.get<ecs::Transform>(id);
            if (!ancestorMoved && own != nullptr && own->parent != newParent) {
                moves.push_back(id);
            }
        }
    }
    for (const ecs::EntityID id : moves) {
        m_host.postFromUi(makeReparentCommand(entityHandle(id), entityHandle(newParent), true));
    }
    if (!moves.empty()) {
        emit sceneEdited();
    }
    return static_cast<int>(moves.size());
}

void HierarchyWidget::onRowsDropped(const QList<QTreeWidgetItem*>& dragged, QTreeWidgetItem* newParent) {
    std::vector<ecs::EntityID> entities;
    for (const QTreeWidgetItem* item : dragged) {
        entities.push_back(itemEntity(item));
    }
    reparentEntities(entities, newParent != nullptr ? itemEntity(newParent) : ecs::EntityID::null());
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

namespace {

constexpr const char* kVecSuffix[3] = {".x", ".y", ".z"};

QDoubleSpinBox* makeSpin(QWidget* parent, const PropertyFieldDesc& desc, int decimals, const QString& name) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(desc.minValue, desc.maxValue);
    spin->setDecimals(decimals);
    spin->setSingleStep(desc.step);
    // Typing posts once on Enter / focus-out; arrows, wheel and drags post every step (the queue
    // and the CommandStack coalesce those into one undo step).
    spin->setKeyboardTracking(false);
    spin->setAccelerated(true);
    spin->setMinimumWidth(56);
    return spin;
}

bool anyHasFocus(const QWidget* root) {
    if (root == nullptr) {
        return false;
    }
    const QWidget* focus = QApplication::focusWidget();
    return focus != nullptr && (focus == root || root->isAncestorOf(focus));
}

void setSpin(QDoubleSpinBox* spin, double value) {
    if (spin != nullptr && spin->value() != value) {
        const QSignalBlocker block(spin);
        spin->setValue(value);
    }
}

} // namespace

InspectorWidget::InspectorWidget(FeaturePaneBridge& bridge, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_bridge(bridge), m_sceneMutex(sceneMutex) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_pane = new PropertyPaneWidget(bridge, this);
    layout->addWidget(m_pane);

    auto* toolRow = new QHBoxLayout();
    m_addButton = new QToolButton(this);
    m_addButton->setObjectName(QStringLiteral("fuseInspectorAddComponent"));
    m_addButton->setText(tr("Add Component"));
    m_addButton->setPopupMode(QToolButton::InstantPopup);
    m_addMenu = new QMenu(m_addButton);
    m_addMenu->setObjectName(QStringLiteral("fuseInspectorAddComponentMenu"));
    m_addButton->setMenu(m_addMenu);
    m_addButton->setEnabled(false);
    toolRow->addWidget(m_addButton);
    toolRow->addStretch(1);
    m_rawToggle = new QToolButton(this);
    m_rawToggle->setObjectName(QStringLiteral("fuseInspectorRawToggle"));
    m_rawToggle->setText(tr("Raw"));
    m_rawToggle->setToolTip(tr("Show every component field as read-only text"));
    m_rawToggle->setCheckable(true);
    toolRow->addWidget(m_rawToggle);
    layout->addLayout(toolRow);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("fuseInspectorScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_editorPage = new QWidget(m_scroll);
    m_editorLayout = new QVBoxLayout(m_editorPage);
    m_editorLayout->setContentsMargins(0, 0, 0, 0);
    m_editorLayout->addStretch(1);
    m_scroll->setWidget(m_editorPage);
    layout->addWidget(m_scroll, 1);

    m_sections = new QTreeWidget(this);
    m_sections->setObjectName(QStringLiteral("fuseInspectorSections"));
    m_sections->setColumnCount(2);
    m_sections->setHeaderLabels({tr("Property"), tr("Value")});
    m_sections->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_sections->setVisible(false);
    layout->addWidget(m_sections, 1);
    connect(m_rawToggle, &QToolButton::toggled, m_sections, &QWidget::setVisible);
}

void InspectorWidget::refresh() {
    std::vector<std::string> signature;
    std::vector<SectionView> sections;
    std::vector<std::pair<const PropertyFieldDesc*, std::string>> values;
    std::string structure;
    ecs::EntityID target = ecs::EntityID::null();
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_pane->refresh();
        EditorHost& host = m_bridge.host();
        m_ecsInspector.sync(host.editorState(), host.editorScene());
        if (m_ecsInspector.hasSelection()) {
            target = m_ecsInspector.target();
            const ecs::Registry& registry = host.editorScene().registry();
            structure = std::to_string(target.index) + ":" + std::to_string(target.generation);
            signature.push_back(structure);
            for (const PropertyInspector::ComponentSection& section : m_ecsInspector.sections()) {
                signature.push_back("#" + section.componentName);
                for (const PropertyInspector::Field& field : section.fields) {
                    signature.push_back(field.name + "=" + field.value);
                }
                SectionView view;
                view.componentName = section.componentName;
                view.kind = findComponentKind(section.componentName);
                if (view.kind != nullptr) {
                    for (const PropertyFieldDesc& field : view.kind->fields) {
                        std::string value;
                        if (readComponentProperty(registry, target, field.propertyName, value)) {
                            values.emplace_back(&field, std::move(value));
                        }
                    }
                } else {
                    view.readOnlyFields = section.fields;
                }
                structure += "#" + section.componentName;
                // Read-only (module) sections rebuild when their values change.
                for (const PropertyInspector::Field& field : view.readOnlyFields) {
                    structure += "|" + field.value;
                }
                sections.push_back(std::move(view));
            }
        }
    }

    m_boundEntity = target;
    if (structure != m_structureKey) {
        m_structureKey = structure;
        rebuildEditors(sections);
    }
    updateEditorValues(values);

    if (signature == m_lastSignature) {
        return; // unchanged: keep the raw tree (and its expansion / scroll state) as is
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

void InspectorWidget::clearEditors() {
    m_bindings.clear();
    m_removeButtons.clear();
    m_shownSections.clear();
    while (m_editorLayout->count() > 1) { // keep the trailing stretch
        QLayoutItem* item = m_editorLayout->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
            widget->hide();
        }
        delete item;
    }
}

void InspectorWidget::rebuildEditors(const std::vector<SectionView>& sections) {
    m_syncing = true;
    clearEditors();
    m_addMenu->clear();
    const bool hasTarget = m_boundEntity.valid();
    m_addButton->setEnabled(hasTarget);

    for (const SectionView& view : sections) {
        const QString name = QString::fromStdString(view.componentName);
        m_shownSections.push_back(view.componentName);
        auto* group = new QGroupBox(m_editorPage);
        group->setObjectName(QStringLiteral("fuseInspectorSection.%1").arg(name));
        group->setTitle(view.kind != nullptr ? tr(view.kind->displayName) : name);
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(6, 4, 6, 6);
        if (view.kind != nullptr && view.kind->removable) {
            auto* header = new QHBoxLayout();
            header->addStretch(1);
            auto* remove = new QToolButton(group);
            remove->setObjectName(QStringLiteral("fuseInspectorRemove.%1").arg(name));
            remove->setText(tr("Remove"));
            remove->setToolTip(tr("Remove the %1 component").arg(name));
            connect(remove, &QToolButton::clicked, this, [this, name]() { removeComponent(name); });
            header->addWidget(remove);
            groupLayout->addLayout(header);
            m_removeButtons.emplace_back(view.componentName, remove);
        }
        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        if (view.kind != nullptr) {
            for (const PropertyFieldDesc& field : view.kind->fields) {
                form->addRow(tr(field.label), makeFieldEditor(field, group));
            }
        } else {
            for (const PropertyInspector::Field& field : view.readOnlyFields) {
                auto* value = new QLabel(QString::fromStdString(field.value), group);
                value->setTextInteractionFlags(Qt::TextSelectableByMouse);
                form->addRow(QString::fromStdString(field.name), value);
            }
        }
        groupLayout->addLayout(form);
        m_editorLayout->insertWidget(m_editorLayout->count() - 1, group);
    }

    if (hasTarget) {
        for (const ComponentKindDesc& kind : editorComponentKinds()) {
            if (!kind.addable ||
                std::find(m_shownSections.begin(), m_shownSections.end(), std::string(kind.name)) !=
                    m_shownSections.end()) {
                continue;
            }
            const QString name = QString::fromLatin1(kind.name);
            QAction* action = m_addMenu->addAction(tr(kind.displayName));
            action->setObjectName(QStringLiteral("fuseInspectorAdd.%1").arg(name));
            action->setData(name);
            connect(action, &QAction::triggered, this, [this, name]() { addComponent(name); });
        }
    }
    m_syncing = false;
}

QWidget* InspectorWidget::makeFieldEditor(const PropertyFieldDesc& desc, QWidget* parent) {
    FieldBinding b;
    b.desc = &desc;
    const QString prop = QString::fromLatin1(desc.propertyName);
    auto* root = new QWidget(parent);
    root->setObjectName(prop);
    auto* row = new QHBoxLayout(root);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    b.root = root;

    switch (desc.type) {
    case PropertyFieldType::Float:
        b.spins[0] = makeSpin(root, desc, 4, prop + QStringLiteral(".value"));
        row->addWidget(b.spins[0]);
        break;
    case PropertyFieldType::UInt:
        b.spins[0] = makeSpin(root, desc, 0, prop + QStringLiteral(".value"));
        b.spins[0]->setSingleStep(1.0);
        row->addWidget(b.spins[0]);
        break;
    case PropertyFieldType::Vec3:
    case PropertyFieldType::Color:
    case PropertyFieldType::Euler:
        for (int i = 0; i < 3; ++i) {
            b.spins[i] = makeSpin(root, desc, desc.type == PropertyFieldType::Euler ? 2 : 4,
                                  prop + QString::fromLatin1(kVecSuffix[i]));
            if (desc.type == PropertyFieldType::Euler) {
                b.spins[i]->setSuffix(QStringLiteral("°"));
                b.spins[i]->setWrapping(true);
                b.spins[i]->setRange(-180.0, 180.0);
            }
            row->addWidget(b.spins[i], 1);
        }
        break;
    case PropertyFieldType::Enum: {
        b.combo = new QComboBox(root);
        b.combo->setObjectName(prop + QStringLiteral(".value"));
        for (u32 i = 0; i < desc.enumCount; ++i) {
            b.combo->addItem(QString::fromLatin1(desc.enumNames[i]));
        }
        row->addWidget(b.combo, 1);
        break;
    }
    case PropertyFieldType::Bool:
        b.check = new QCheckBox(root);
        b.check->setObjectName(prop + QStringLiteral(".value"));
        row->addWidget(b.check);
        row->addStretch(1);
        break;
    case PropertyFieldType::String:
    case PropertyFieldType::AssetPath:
        b.text = new QLineEdit(root);
        b.text->setObjectName(prop + QStringLiteral(".value"));
        row->addWidget(b.text, 1);
        break;
    }

    m_bindings.push_back(b);
    const usize index = m_bindings.size() - 1u;
    const auto post = [this, index]() {
        if (!m_syncing && index < m_bindings.size()) {
            postField(m_bindings[index]);
        }
    };
    for (QDoubleSpinBox* spin : b.spins) {
        if (spin != nullptr) {
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, post);
        }
    }
    if (b.combo != nullptr) {
        connect(b.combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, post);
    }
    if (b.check != nullptr) {
        connect(b.check, &QCheckBox::toggled, this, post);
    }
    if (b.text != nullptr) {
        connect(b.text, &QLineEdit::editingFinished, this, post);
    }
    if (desc.type == PropertyFieldType::Color) {
        auto* swatch = new QToolButton(root);
        swatch->setObjectName(prop + QStringLiteral(".pick"));
        swatch->setText(QStringLiteral("■"));
        swatch->setToolTip(tr("Pick a colour"));
        connect(swatch, &QToolButton::clicked, this, [this, index]() {
            if (index < m_bindings.size()) {
                chooseColor(m_bindings[index]);
            }
        });
        row->addWidget(swatch);
    }
    if (desc.type == PropertyFieldType::AssetPath) {
        auto* browse = new QToolButton(root);
        browse->setObjectName(prop + QStringLiteral(".browse"));
        browse->setText(QStringLiteral("…"));
        browse->setToolTip(tr("Choose an asset"));
        connect(browse, &QToolButton::clicked, this, [this, index]() {
            if (index < m_bindings.size()) {
                chooseAsset(m_bindings[index]);
            }
        });
        row->addWidget(browse);
    }
    return root;
}

void InspectorWidget::updateEditorValues(const std::vector<std::pair<const PropertyFieldDesc*, std::string>>& values) {
    m_syncing = true;
    for (const auto& [desc, text] : values) {
        const FieldBinding* found = nullptr;
        for (const FieldBinding& b : m_bindings) {
            if (b.desc == desc) {
                found = &b;
                break;
            }
        }
        if (found == nullptr || anyHasFocus(found->root)) {
            continue; // never overwrite a field the user is editing
        }
        const FieldBinding& b = *found;
        switch (desc->type) {
        case PropertyFieldType::Float:
        case PropertyFieldType::UInt: {
            setSpin(b.spins[0], std::strtod(text.c_str(), nullptr));
            break;
        }
        case PropertyFieldType::Vec3:
        case PropertyFieldType::Color: {
            f32 v[3]{};
            if (parsePropertyFloats(text, v, 3u)) {
                for (int i = 0; i < 3; ++i) {
                    setSpin(b.spins[i], v[i]);
                }
            }
            break;
        }
        case PropertyFieldType::Euler: {
            f32 q[4]{};
            if (!parsePropertyFloats(text, q, 4u)) {
                break;
            }
            const ecs::quat stored{q[0], q[1], q[2], q[3]};
            // Keep the shown angles when they already describe the stored rotation (Euler angles
            // are not unique; re-deriving them would make the spin boxes jump while editing).
            const ecs::quat shown = quatFromEulerDeg(static_cast<f32>(b.spins[0]->value()),
                                                     static_cast<f32>(b.spins[1]->value()),
                                                     static_cast<f32>(b.spins[2]->value()));
            const f32 dot = std::fabs(shown.x * stored.x + shown.y * stored.y + shown.z * stored.z + shown.w * stored.w);
            if (dot > 0.99999f) {
                break;
            }
            const ecs::vec3 euler = eulerDegFromQuat(stored);
            setSpin(b.spins[0], euler.x);
            setSpin(b.spins[1], euler.y);
            setSpin(b.spins[2], euler.z);
            break;
        }
        case PropertyFieldType::Enum: {
            const int index = static_cast<int>(std::strtoul(text.c_str(), nullptr, 10));
            if (b.combo->currentIndex() != index) {
                const QSignalBlocker block(b.combo);
                b.combo->setCurrentIndex(index);
            }
            break;
        }
        case PropertyFieldType::Bool: {
            const bool on = text == "1";
            if (b.check->isChecked() != on) {
                const QSignalBlocker block(b.check);
                b.check->setChecked(on);
            }
            break;
        }
        case PropertyFieldType::String:
        case PropertyFieldType::AssetPath: {
            const QString value = text == kPropertyEmptyString ? QString() : QString::fromStdString(text);
            if (b.text->text() != value) {
                const QSignalBlocker block(b.text);
                b.text->setText(value);
            }
            break;
        }
        }
    }
    m_syncing = false;
}

void InspectorWidget::postField(const FieldBinding& b) {
    if (!m_boundEntity.valid() || b.desc == nullptr) {
        return;
    }
    std::string value;
    switch (b.desc->type) {
    case PropertyFieldType::Float:
        value = formatPropertyFloat(static_cast<f32>(b.spins[0]->value()));
        break;
    case PropertyFieldType::UInt:
        value = std::to_string(static_cast<unsigned long long>(std::llround(b.spins[0]->value())));
        break;
    case PropertyFieldType::Vec3:
    case PropertyFieldType::Color:
        value = formatPropertyFloat(static_cast<f32>(b.spins[0]->value())) + "," +
                formatPropertyFloat(static_cast<f32>(b.spins[1]->value())) + "," +
                formatPropertyFloat(static_cast<f32>(b.spins[2]->value()));
        break;
    case PropertyFieldType::Euler:
        value = formatPropertyQuat(quatFromEulerDeg(static_cast<f32>(b.spins[0]->value()),
                                                    static_cast<f32>(b.spins[1]->value()),
                                                    static_cast<f32>(b.spins[2]->value())));
        break;
    case PropertyFieldType::Enum:
        value = std::to_string(b.combo->currentIndex() < 0 ? 0 : b.combo->currentIndex());
        break;
    case PropertyFieldType::Bool:
        value = b.check->isChecked() ? "1" : "0";
        break;
    case PropertyFieldType::String:
    case PropertyFieldType::AssetPath:
        value = b.text->text().toStdString();
        if (value.empty()) {
            value = kPropertyEmptyString;
        }
        break;
    }
    m_bridge.host().postFromUi(makeSetPropertyCommand(entityHandle(m_boundEntity), b.desc->propertyName, value));
    emit propertyEdited();
}

void InspectorWidget::chooseColor(const FieldBinding& b) {
    const QColor initial = QColor::fromRgbF(static_cast<float>(std::clamp(b.spins[0]->value(), 0.0, 1.0)),
                                            static_cast<float>(std::clamp(b.spins[1]->value(), 0.0, 1.0)),
                                            static_cast<float>(std::clamp(b.spins[2]->value(), 0.0, 1.0)));
    const QColor picked = QColorDialog::getColor(initial, this, tr("Pick a colour"));
    if (!picked.isValid()) {
        return;
    }
    m_syncing = true;
    setSpin(b.spins[0], picked.redF());
    setSpin(b.spins[1], picked.greenF());
    setSpin(b.spins[2], picked.blueF());
    m_syncing = false;
    postField(b);
}

void InspectorWidget::chooseAsset(const FieldBinding& b) {
    const EditorHost& host = m_bridge.host();
    const QString root = host.hasProject() ? QString::fromStdString(host.projectManifest().projectRoot) : QString();
    const QString filter = b.desc->assetFilter != nullptr ? tr(b.desc->assetFilter) : tr("All files (*)");
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose asset"), root, filter);
    if (file.isEmpty()) {
        return;
    }
    // Project-relative when the asset lives inside the open project.
    QString path = file;
    if (!root.isEmpty()) {
        const QString relative = QDir(root).relativeFilePath(file);
        if (!relative.startsWith(QStringLiteral(".."))) {
            path = relative;
        }
    }
    {
        const QSignalBlocker block(b.text);
        b.text->setText(path);
    }
    postField(b);
}

const InspectorWidget::FieldBinding* InspectorWidget::binding(const QString& propertyName) const {
    for (const FieldBinding& b : m_bindings) {
        if (propertyName == QLatin1String(b.desc->propertyName)) {
            return &b;
        }
    }
    return nullptr;
}

QStringList InspectorWidget::componentSections() const {
    QStringList names;
    for (const std::string& name : m_shownSections) {
        names << QString::fromStdString(name);
    }
    return names;
}

QWidget* InspectorWidget::fieldEditor(const QString& propertyName) const {
    const FieldBinding* b = binding(propertyName);
    return b != nullptr ? b->root : nullptr;
}

QDoubleSpinBox* InspectorWidget::fieldSpin(const QString& propertyName, int component) const {
    const FieldBinding* b = binding(propertyName);
    return b != nullptr && component >= 0 && component < 3 ? b->spins[component] : nullptr;
}

QCheckBox* InspectorWidget::fieldCheck(const QString& propertyName) const {
    const FieldBinding* b = binding(propertyName);
    return b != nullptr ? b->check : nullptr;
}

QComboBox* InspectorWidget::fieldCombo(const QString& propertyName) const {
    const FieldBinding* b = binding(propertyName);
    return b != nullptr ? b->combo : nullptr;
}

QLineEdit* InspectorWidget::fieldText(const QString& propertyName) const {
    const FieldBinding* b = binding(propertyName);
    return b != nullptr ? b->text : nullptr;
}

QToolButton* InspectorWidget::removeButton(const QString& componentName) const {
    for (const auto& [name, button] : m_removeButtons) {
        if (componentName == QString::fromStdString(name)) {
            return button;
        }
    }
    return nullptr;
}

bool InspectorWidget::addComponent(const QString& componentName) {
    if (!m_boundEntity.valid() || findComponentKind(componentName.toStdString()) == nullptr) {
        return false;
    }
    m_bridge.host().postFromUi(makeAddComponentCommand(entityHandle(m_boundEntity), componentName.toStdString()));
    emit componentsChanged();
    return true;
}

bool InspectorWidget::removeComponent(const QString& componentName) {
    const ComponentKindDesc* kind = findComponentKind(componentName.toStdString());
    if (!m_boundEntity.valid() || kind == nullptr || !kind->removable) {
        return false;
    }
    m_bridge.host().postFromUi(makeRemoveComponentCommand(entityHandle(m_boundEntity), componentName.toStdString()));
    emit componentsChanged();
    return true;
}

// ---- ConsoleWidget --------------------------------------------------------------------------------

namespace {

constexpr fuse::log::Level kConsoleLevels[5] = {fuse::log::Level::Trace, fuse::log::Level::Debug,
                                                fuse::log::Level::Info, fuse::log::Level::Warn,
                                                fuse::log::Level::Error};

int consoleLevelSlot(fuse::log::Level level) {
    for (int i = 0; i < 5; ++i) {
        if (kConsoleLevels[i] == level) {
            return i;
        }
    }
    return -1;
}

} // namespace

ConsoleWidget::ConsoleWidget(QWidget* parent) : ConsoleWidget(nullptr, parent) {}

ConsoleWidget::ConsoleWidget(EditorHost* host, QWidget* parent) : QWidget(parent), m_host(host) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* filterRow = new QHBoxLayout();
    filterRow->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 5; ++i) {
        auto* button = new QToolButton(this);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setObjectName(QStringLiteral("fuseConsoleLevel%1").arg(QString::fromLatin1(ConsolePanel::levelLabel(kConsoleLevels[i]))));
        m_levelButtons[i] = button;
        filterRow->addWidget(button);
        connect(button, &QToolButton::toggled, this, [this](bool) { onFiltersChanged(); });
    }
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("fuseConsoleFilter"));
    m_filter->setPlaceholderText(tr("Filter…"));
    m_filter->setClearButtonEnabled(true);
    filterRow->addWidget(m_filter, 1);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString&) { onFiltersChanged(); });
    layout->addLayout(filterRow);

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("fuseConsoleLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    layout->addWidget(m_log, 1);
    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("fuseConsoleInput"));
    m_input->setPlaceholderText(m_host != nullptr ? tr("Command or Lua (help)") : tr("Command (help)"));
    m_input->installEventFilter(this);
    layout->addWidget(m_input);
    connect(m_input, &QLineEdit::returnPressed, this, &ConsoleWidget::onSubmit);

    if (m_host != nullptr) {
        registerEditorConsoleCommands(m_panel, *m_host);
    }
    // Initial button state mirrors the panel's filters (Trace hidden by default).
    const bool shown[5] = {m_panel.showTrace(), m_panel.showDebug(), m_panel.showInfo(), m_panel.showWarnings(),
                           m_panel.showErrors()};
    for (int i = 0; i < 5; ++i) {
        const QSignalBlocker block(m_levelButtons[i]);
        m_levelButtons[i]->setChecked(shown[i]);
    }

    fuse::log::Logger::instance().setSink(&ConsoleWidget::logSink, this);
    m_panel.addLog(fuse::log::Level::Info, "FUSE editor console ready");
    refreshLevelButtons();
    rerender();
}

ConsoleWidget::~ConsoleWidget() {
    fuse::log::Logger::instance().setSink(nullptr, nullptr);
}

QToolButton* ConsoleWidget::levelButton(fuse::log::Level level) const {
    const int slot = consoleLevelSlot(level);
    return slot >= 0 ? m_levelButtons[slot] : nullptr;
}

QColor ConsoleWidget::levelColor(fuse::log::Level level) {
    switch (level) {
    case fuse::log::Level::Trace:
        return QColor(0x80, 0x80, 0x80);
    case fuse::log::Level::Debug:
        return QColor(0x6f, 0xa8, 0xdc);
    case fuse::log::Level::Warn:
        return QColor(0xe5, 0xb4, 0x3c);
    case fuse::log::Level::Error:
    case fuse::log::Level::Fatal:
        return QColor(0xf0, 0x5a, 0x5a);
    case fuse::log::Level::Info:
    default:
        return QColor(0xd4, 0xd4, 0xd4);
    }
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
    usize hostLines = 0;
    if (m_host != nullptr) {
        hostLines = m_host->drainConsoleOutput(m_panel);
    }
    if (pending.empty() && hostLines == 0u) {
        return;
    }
    for (const auto& [level, text] : pending) {
        m_panel.addLog(level, text.c_str());
    }
    refreshLevelButtons();
    rerender();
}

void ConsoleWidget::appendLine(const ConsolePanel::LogLine& line) {
    QString text = QStringLiteral("[%1] %2").arg(QString::fromLatin1(ConsolePanel::levelLabel(line.level)),
                                                QString::fromStdString(line.text));
    if (line.repeatCount > 1) {
        text += QStringLiteral(" (x%1)").arg(line.repeatCount);
    }
    m_log->appendHtml(QStringLiteral("<span style=\"color:%1; white-space:pre-wrap;\">%2</span>")
                          .arg(levelColor(line.level).name(), text.toHtmlEscaped()));
}

void ConsoleWidget::rerender() {
    const std::vector<ConsolePanel::LogLine> lines = m_panel.filteredLines();
    const u32 tailRepeat = lines.empty() ? 0u : lines.back().repeatCount;
    const bool tailChanged = lines.size() == m_renderedLines && tailRepeat != m_renderedTailRepeat;
    if (lines.size() < m_renderedLines || tailChanged) {
        // Filter change, clear, or a repeat count bump on the last line: rebuild the view.
        m_log->clear();
        m_renderedLines = 0;
    } else if (m_renderedLines > 0 && m_renderedLines == lines.size()) {
        return;
    } else if (m_renderedLines > 0 && lines.size() > m_renderedLines &&
               lines[m_renderedLines - 1u].repeatCount != m_renderedTailRepeat) {
        // The previously last line repeated before new lines arrived: its "(xN)" is stale.
        m_log->clear();
        m_renderedLines = 0;
    }
    for (usize i = m_renderedLines; i < lines.size(); ++i) {
        appendLine(lines[i]);
    }
    m_renderedLines = lines.size();
    m_renderedTailRepeat = tailRepeat;
}

void ConsoleWidget::onFiltersChanged() {
    m_panel.setShowTrace(m_levelButtons[0]->isChecked());
    m_panel.setShowDebug(m_levelButtons[1]->isChecked());
    m_panel.setShowInfo(m_levelButtons[2]->isChecked());
    m_panel.setShowWarnings(m_levelButtons[3]->isChecked());
    m_panel.setShowErrors(m_levelButtons[4]->isChecked());
    m_panel.setTextFilter(m_filter->text().toUtf8().constData());
    m_log->clear();
    m_renderedLines = 0;
    rerender();
}

void ConsoleWidget::refreshLevelButtons() {
    for (int i = 0; i < 5; ++i) {
        const fuse::log::Level level = kConsoleLevels[i];
        m_levelButtons[i]->setText(QStringLiteral("%1 (%2)")
                                       .arg(QString::fromLatin1(ConsolePanel::levelLabel(level)))
                                       .arg(m_panel.levelCount(level)));
        QPalette palette = m_levelButtons[i]->palette();
        palette.setColor(QPalette::ButtonText, levelColor(level));
        m_levelButtons[i]->setPalette(palette);
    }
}

bool ConsoleWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Up) {
            const std::string previous = m_panel.historyPrevious();
            if (!previous.empty()) {
                m_input->setText(QString::fromStdString(previous));
            }
            return true;
        }
        if (key->key() == Qt::Key_Down) {
            m_input->setText(QString::fromStdString(m_panel.historyNext()));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ConsoleWidget::onSubmit() {
    const QByteArray line = m_input->text().toUtf8();
    m_input->clear();
    m_panel.executeCommand(line.constData());
    refreshLevelButtons();
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
