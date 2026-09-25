#include "editor_theme.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOption>
#include <QStyleFactory>

namespace fuse::editor::qt {

namespace {

/// Fusion with the spec's DockingPreview for rubber bands (the dock drop / gap indicator), which
/// Fusion otherwise derives from the highlight colour and stylesheets cannot address.
class EditorStyle final : public QProxyStyle {
public:
    EditorStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override {
        if (element == CE_RubberBand) {
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->fillRect(option->rect, themeColor("DockingPreview"));
            painter->restore();
            return;
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};

QColor toColor(const ThemeColorSpec& spec) {
    // 8-bit quantisation (round to nearest), the precision of stylesheet colours; QColor::fromRgbF
    // keeps 16 bits, which would make palette and stylesheet colours disagree by one step.
    auto q = [](float v) { return static_cast<int>(v * 255.f + 0.5f); };
    return QColor(q(spec.r), q(spec.g), q(spec.b), q(spec.a));
}

QString css(std::string_view role) {
    const QColor c = themeColor(role);
    if (c.alpha() == 255) {
        return c.name(QColor::HexRgb);
    }
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

} // namespace

QColor themeColor(std::string_view role) {
    for (const ThemeColorSpec& spec : kDarkThemeSpec) {
        if (role == spec.role) {
            return toColor(spec);
        }
    }
    return {};
}

QPalette darkPalette() {
    QPalette p;
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(group, QPalette::Window, themeColor("WindowBg"));
        p.setColor(group, QPalette::Base, themeColor("FrameBg"));
        p.setColor(group, QPalette::AlternateBase, themeColor("ChildBg"));
        p.setColor(group, QPalette::ToolTipBase, themeColor("PopupBg"));
        p.setColor(group, QPalette::Button, themeColor("Button"));
        p.setColor(group, QPalette::Light, themeColor("FrameBgActive"));
        p.setColor(group, QPalette::Midlight, themeColor("FrameBgHovered"));
        p.setColor(group, QPalette::Mid, themeColor("Border"));
        p.setColor(group, QPalette::Dark, themeColor("Border"));
        p.setColor(group, QPalette::Shadow, themeColor("ChildBg"));
        p.setColor(group, QPalette::Highlight, themeColor("SliderGrab"));
        p.setColor(group, QPalette::Link, themeColor("CheckMark"));
        p.setColor(group, QPalette::PlaceholderText, themeColor("TextDisabled"));
        const QColor text = group == QPalette::Disabled ? themeColor("TextDisabled") : themeColor("Text");
        p.setColor(group, QPalette::WindowText, text);
        p.setColor(group, QPalette::Text, text);
        p.setColor(group, QPalette::ButtonText, text);
        p.setColor(group, QPalette::ToolTipText, text);
        p.setColor(group, QPalette::HighlightedText, themeColor("Text"));
        p.setColor(group, QPalette::BrightText, QColor(255, 255, 255));
    }
    return p;
}

QString darkStyleSheet() {
    // Rounding / border sizes follow the P6 style block (WindowBorderSize 1, FrameRounding 3,
    // FrameBorderSize 0, ItemSpacing 6x4, WindowPadding 8).
    QString s;
    s += QStringLiteral("QMainWindow { background-color: %1; }\n").arg(css("WindowBg"));
    s += QStringLiteral("QMainWindow::separator { background-color: %1; width: 4px; height: 4px; }\n")
             .arg(css("Separator"));
    s += QStringLiteral("QMainWindow::separator:hover { background-color: %1; }\n").arg(css("HeaderHovered"));
    s += QStringLiteral("QSplitter::handle { background-color: %1; }\n").arg(css("Separator"));
    s += QStringLiteral("QRubberBand { background-color: %1; }\n").arg(css("DockingPreview"));

    s += QStringLiteral("QToolTip { background-color: %1; color: %2; border: 1px solid %3; }\n")
             .arg(css("PopupBg"), css("Text"), css("Border"));

    s += QStringLiteral("QMenuBar { background-color: %1; color: %2; }\n").arg(css("MenuBarBg"), css("Text"));
    s += QStringLiteral("QMenuBar::item { background: transparent; padding: 4px 8px; }\n");
    s += QStringLiteral("QMenuBar::item:selected { background-color: %1; }\n").arg(css("HeaderHovered"));
    s += QStringLiteral("QMenuBar::item:pressed { background-color: %1; }\n").arg(css("HeaderActive"));
    // Menu metrics match fuse::editor::EntityContextMenu (kMenuPadding 4, kItemHeight 22,
    // kSeparatorHeight 7) so the headless placement model and QMenu geometry agree.
    s += QStringLiteral("QMenu { background-color: %1; color: %2; border: 0px; padding: 4px 0px; }\n")
             .arg(css("PopupBg"), css("Text"));
    s += QStringLiteral("QMenu::item { padding: 0px 24px 0px 12px; margin: 0px; border: 0px; height: 22px; }\n");
    s += QStringLiteral("QMenu::item:selected { background-color: %1; }\n").arg(css("HeaderHovered"));
    s += QStringLiteral("QMenu::item:disabled { color: %1; }\n").arg(css("TextDisabled"));
    s += QStringLiteral("QMenu::separator { height: 1px; margin: 3px 0px; background-color: %1; }\n")
             .arg(css("Separator"));

    s += QStringLiteral("QDockWidget { color: %1; }\n").arg(css("Text"));
    s += QStringLiteral("QDockWidget::title { background-color: %1; padding: 4px 8px; text-align: left; }\n")
             .arg(css("TitleBg"));
    s += QStringLiteral("QDockWidget[fuseActive=\"true\"]::title { background-color: %1; }\n")
             .arg(css("TitleBgActive"));

    s += QStringLiteral("QPushButton { background-color: %1; color: %2; border: 1px solid %3; "
                        "border-radius: 3px; padding: 3px 10px; }\n")
             .arg(css("Button"), css("Text"), css("Border"));
    s += QStringLiteral("QPushButton:hover { background-color: %1; }\n").arg(css("ButtonHovered"));
    s += QStringLiteral("QPushButton:pressed { background-color: %1; }\n").arg(css("ButtonActive"));
    s += QStringLiteral("QPushButton:disabled { color: %1; }\n").arg(css("TextDisabled"));

    const QString frames = QStringLiteral("QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox");
    s += QStringLiteral("%1 { background-color: %2; color: %3; border: 0px; border-radius: 3px; padding: 2px 4px; "
                        "selection-background-color: %4; }\n")
             .arg(frames, css("FrameBg"), css("Text"), css("SliderGrab"));
    s += QStringLiteral("QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover "
                        "{ background-color: %1; }\n")
             .arg(css("FrameBgHovered"));
    s += QStringLiteral("QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus "
                        "{ background-color: %1; }\n")
             .arg(css("FrameBgActive"));
    s += QStringLiteral("QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: %1; }\n")
             .arg(css("TextDisabled"));

    s += QStringLiteral("QAbstractItemView, QPlainTextEdit, QTextEdit { background-color: %1; color: %2; "
                        "border: 1px solid %3; selection-background-color: %4; }\n")
             .arg(css("ChildBg"), css("Text"), css("Border"), css("HeaderActive"));
    s += QStringLiteral("QAbstractItemView::item:hover { background-color: %1; }\n").arg(css("HeaderHovered"));
    s += QStringLiteral("QAbstractItemView::item:selected { background-color: %1; color: %2; }\n")
             .arg(css("Header"), css("Text"));
    // HeaderActive: the selected current row of the focused view (the "active" header role).
    s += QStringLiteral("QAbstractItemView::item:selected:focus { background-color: %1; }\n")
             .arg(css("HeaderActive"));
    s += QStringLiteral("QHeaderView::section { background-color: %1; color: %2; border: 0px; padding: 2px 6px; }\n")
             .arg(css("Header"), css("Text"));

    s += QStringLiteral("QTabBar::tab { background-color: %1; color: %2; padding: 4px 10px; border: 0px; "
                        "border-top-left-radius: 3px; border-top-right-radius: 3px; }\n")
             .arg(css("Tab"), css("Text"));
    s += QStringLiteral("QTabBar::tab:hover { background-color: %1; }\n").arg(css("TabHovered"));
    s += QStringLiteral("QTabBar::tab:selected { background-color: %1; }\n").arg(css("TabActive"));

    s += QStringLiteral("QCheckBox::indicator { width: 13px; height: 13px; background-color: %1; border-radius: 3px; }\n")
             .arg(css("FrameBg"));
    s += QStringLiteral("QCheckBox::indicator:checked { background-color: %1; }\n").arg(css("CheckMark"));

    s += QStringLiteral("QSlider::groove:horizontal { background-color: %1; height: 6px; border-radius: 3px; }\n")
             .arg(css("FrameBg"));
    s += QStringLiteral("QSlider::handle:horizontal { background-color: %1; width: 12px; margin: -4px 0px; "
                        "border-radius: 3px; }\n")
             .arg(css("SliderGrab"));
    s += QStringLiteral("QSlider::handle:horizontal:pressed { background-color: %1; }\n").arg(css("SliderGrabActive"));

    s += QStringLiteral("QScrollBar { background-color: %1; }\n").arg(css("ChildBg"));
    s += QStringLiteral("QScrollBar::handle { background-color: %1; border-radius: 3px; }\n").arg(css("Header"));
    s += QStringLiteral("QStatusBar { background-color: %1; color: %2; }\n").arg(css("MenuBarBg"), css("Text"));
    s += QStringLiteral("QFrame[frameShape=\"4\"], QFrame[frameShape=\"5\"] { color: %1; }\n").arg(css("Separator"));
    return s;
}

QString loadEditorFont() {
    static const QString family = []() -> QString {
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fuse/editor/fonts/OpenSans.ttf"));
        if (id < 0) {
            return {};
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        return families.isEmpty() ? QString() : families.front();
    }();
    return family;
}

QFont editorFont() {
    QFont font(loadEditorFont());
    font.setPixelSize(kEditorFontPixelSize);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

void applyEditorTheme(QApplication& app) {
    QApplication::setStyle(new EditorStyle());
    QApplication::setPalette(darkPalette());
    if (!loadEditorFont().isEmpty()) {
        QApplication::setFont(editorFont());
    }
    app.setStyleSheet(darkStyleSheet());
}

} // namespace fuse::editor::qt
