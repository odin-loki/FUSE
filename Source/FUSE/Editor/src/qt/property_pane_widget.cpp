#include "property_pane_widget.hpp"

#include <fuse/editor/editor_host.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>

namespace fuse::editor::qt {

PropertyPaneWidget::PropertyPaneWidget(FeaturePaneBridge& bridge, QWidget* parent)
    : QWidget(parent), m_bridge(bridge) {
    auto* layout = new QVBoxLayout(this);

    m_summaryLabel = new QLabel(tr("Property pane — no selection"), this);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    auto* positionForm = new QFormLayout();
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setEnabled(false);
    connect(m_nameEdit, &QLineEdit::editingFinished, this, &PropertyPaneWidget::onNameEdited);
    positionForm->addRow(tr("Name"), m_nameEdit);

    m_posX = new QDoubleSpinBox(this);
    m_posY = new QDoubleSpinBox(this);
    m_posZ = new QDoubleSpinBox(this);
    for (QDoubleSpinBox* spin : {m_posX, m_posY, m_posZ}) {
        spin->setRange(-100000.0, 100000.0);
        spin->setDecimals(3);
        spin->setEnabled(false);
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                &PropertyPaneWidget::onPositionEdited);
    }
    positionForm->addRow(tr("Position X"), m_posX);
    positionForm->addRow(tr("Position Y"), m_posY);
    positionForm->addRow(tr("Position Z"), m_posZ);
    layout->addLayout(positionForm);

    m_playButton = new QPushButton(tr("Play (PIE)"), this);
    m_stopButton = new QPushButton(tr("Stop"), this);
    layout->addWidget(m_playButton);
    layout->addWidget(m_stopButton);
    layout->addStretch(1);

    connect(m_playButton, &QPushButton::clicked, this, &PropertyPaneWidget::onPlayClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &PropertyPaneWidget::onStopClicked);

    refresh();
}

void PropertyPaneWidget::refresh() {
    m_bridge.syncPropertyPane();
    const EditorHost& host = m_bridge.host();
    const PropertyInspector& inspector = m_bridge.propertyInspector();

    QString summary = tr("Project: %1 | Playing: %2 | Sections: %3")
                          .arg(QString::fromStdString(host.loadedProject()))
                          .arg(host.editorState().playing ? tr("yes") : tr("no"))
                          .arg(inspector.hasSelection() ? static_cast<int>(inspector.sections().size()) : 0);
    m_summaryLabel->setText(summary);
    syncNameField();
    syncPositionFields();
}

void PropertyPaneWidget::syncNameField() {
    m_syncingFields = true;

    const PropertyInspector& inspector = m_bridge.propertyInspector();
    std::string name;
    const bool hasName = inspector.getName(m_bridge.host().runtimeScene(), name);
    m_nameEdit->setEnabled(hasName);
    if (hasName) {
        m_nameEdit->setText(QString::fromStdString(name));
    } else {
        m_nameEdit->clear();
    }

    m_syncingFields = false;
}

void PropertyPaneWidget::syncPositionFields() {
    m_syncingFields = true;

    const PropertyInspector& inspector = m_bridge.propertyInspector();
    const bool hasTransform = inspector.hasSelection() &&
                              m_bridge.host().editorScene().registry().has<ecs::Transform>(
                                  inspector.target());

    for (QDoubleSpinBox* spin : {m_posX, m_posY, m_posZ}) {
        spin->setEnabled(hasTransform);
    }

    if (hasTransform) {
        const ecs::Transform* transform =
            m_bridge.host().editorScene().registry().get<ecs::Transform>(inspector.target());
        if (transform != nullptr) {
            m_posX->setValue(transform->position.x);
            m_posY->setValue(transform->position.y);
            m_posZ->setValue(transform->position.z);
        }
    } else {
        m_posX->setValue(0.0);
        m_posY->setValue(0.0);
        m_posZ->setValue(0.0);
    }

    m_syncingFields = false;
}

void PropertyPaneWidget::onNameEdited() {
    if (m_syncingFields) {
        return;
    }

    const PropertyInspector& inspector = m_bridge.propertyInspector();
    if (!inspector.hasSelection()) {
        return;
    }

    m_bridge.postSetProperty(inspector.target(), "name", m_nameEdit->text().toStdString());
}

void PropertyPaneWidget::onPositionEdited() {
    if (m_syncingFields) {
        return;
    }

    const PropertyInspector& inspector = m_bridge.propertyInspector();
    if (!inspector.hasSelection()) {
        return;
    }

    const std::string value = std::to_string(m_posX->value()) + "," + std::to_string(m_posY->value()) +
                              "," + std::to_string(m_posZ->value());
    m_bridge.postSetProperty(inspector.target(), "transform.position", value);
}

void PropertyPaneWidget::onPlayClicked() {
    m_bridge.postPlayRequested();
    refresh();
}

void PropertyPaneWidget::onStopClicked() {
    m_bridge.postStopRequested();
    refresh();
}

} // namespace fuse::editor::qt
