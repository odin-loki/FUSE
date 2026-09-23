// B6.1 / B6.13 gates on the Qt 6 editor host (fuse_editor_qt — the code fuse_editor runs).
//
// One binary, one gate per invocation (`fuse_editor_qt_gates <gate> [arg]`), registered as separate
// ctest entries labelled "gate;qt". Each gate prints its measurements and returns 0 (pass), 1 (fail)
// or 77 (skip: no display / budget not enforced in this build).
//
//   startup         editor initialises all panels + dockspace in < 1 s
//   layout          default layout: viewport, hierarchy, inspector, console visible
//   theme           dark theme: every spec colour (docs/sources/P6.md set_dark_theme) applied
//   font <scale>    bundled font loaded; glyph metrics scale with the device pixel ratio; no
//                   missing glyphs (run under QT_SCALE_FACTOR=<scale>)
//   wasd            free camera: same-frame key response, constant motion per frame, no jitter
//   context_menu    QMenu::popup geometry == EntityContextMenu::placement() (incl. edge flips, DPR)
//   fuse_api        B6.1: Qt panels drive the FUSE editor APIs (undo stack, selection, inspector,
//                   console, PIE) — the host never mutates engine state behind them
//   ui_frame        editor UI render time < 2 ms / frame (Qt paint; budget enforced in Release only)
//   idle_frames     no frame spikes over 10,000 non-interactive frames; editor memory < 256 MiB
//   present_adopt   QVulkanInstance adopts the FUSE VkInstance; Qt surface wired to a real swapchain, 0 validation errors
//   live_present    the real editor (game thread running) presents its viewport through the embedded Vulkan
//                   child window: acquire / present counts grow, the window pixels are the rendered frame (not
//                   the placeholder), resize recreates the swapchain, forwarded input drives the camera /
//                   context menu, 0 validation messages, clean teardown

#include "editor_panels.hpp"
#include "editor_theme.hpp"
#include "main_window.hpp"
#include "property_pane_widget.hpp"
#include "viewport_placeholder_widget.hpp"
#include "viewport_vulkan_window.hpp"

#include <fuse/editor/viewport_swapchain_wiring.hpp>
#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <QVulkanInstance>
#include <QWindow>
#include <vulkan/vulkan.h>
#endif

#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/core/track_b.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRubberBand>
#include <QRawFont>
#include <QScreen>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTest>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <malloc.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

using fuse::editor::qt::MainWindow;
using fuse::editor::qt::ViewportPlaceholderWidget;

namespace {

int g_failures = 0;
long long g_rssBeforeQt = 0; ///< RSS after fuse::core::initialize, before QApplication

void expect(bool condition, const std::string& message) {
    std::printf("%s: %s\n", condition ? "ok  " : "FAIL", message.c_str());
    if (!condition) {
        ++g_failures;
    }
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof(buf), f, args);
    va_end(args);
    return buf;
}

QString samplesRoot() {
    return QStringLiteral(FUSE_TEST_SOURCE_DIR "/Samples/unification");
}

MainWindow::Options quietOptions() {
    MainWindow::Options o;
    o.startGameLoop = false;
    o.startFramePump = false;
    return o;
}

bool showAndExpose(QWidget& w) {
    w.show();
    const bool exposed = QTest::qWaitForWindowExposed(&w, 5000);
    QCoreApplication::processEvents();
    return exposed;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const std::size_t idx = std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    return v[idx];
}

bool releaseBuild() {
#if defined(NDEBUG)
    return true;
#else
    return false;
#endif
}

long long residentBytes() {
    long pages = 0;
    long resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) {
            resident = 0;
        }
        std::fclose(f);
    }
    return static_cast<long long>(resident) * sysconf(_SC_PAGESIZE);
}

long long heapInUse() {
    const struct mallinfo2 mi = mallinfo2();
    return static_cast<long long>(mi.uordblks + mi.hblkhd);
}

long long threadCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

/// Visible on screen: shown, inside a shown window and not covered (e.g. by a raised tab sibling).
bool reallyVisible(QWidget* w) {
    return w != nullptr && w->isVisible() && !w->visibleRegion().isEmpty() && w->width() > 0 && w->height() > 0;
}

// ---- startup ----------------------------------------------------------------------------------------

int gateStartup(double appInitMs) {
    QElapsedTimer t;
    t.start();
    auto window = std::make_unique<MainWindow>(samplesRoot()); // real options: game loop + frame pump
    const double constructMs = static_cast<double>(t.nsecsElapsed()) * 1e-6;
    const bool exposed = showAndExpose(*window);
    // All panels initialised: every dock carries its panel widget, the dockspace has placed it, and
    // everything the default layout shows has been laid out and painted once.
    bool allPlaced = true;
    for (QDockWidget* dock : window->docks()) {
        allPlaced = allPlaced && dock->widget() != nullptr && window->dockWidgetArea(dock) != Qt::NoDockWidgetArea;
    }
    window->repaint();
    const double totalMs = static_cast<double>(t.nsecsElapsed()) * 1e-6;
    std::printf("startup: QApplication+theme %.1f ms, MainWindow ctor %.1f ms, shown+exposed+painted %.1f ms, "
                "total %.1f ms (%zu docks)\n",
                appInitMs, constructMs, totalMs, appInitMs + totalMs, window->docks().size());
    expect(exposed, "main window exposed");
    expect(window->docks().size() == 9u, "all 9 panels (hierarchy, projects, assets, inspector, material, sdf, "
                                         "console, profiler, sequencer) created as docks");
    expect(allPlaced, "every dock has its panel widget and a dock area");
    expect(window->centralWidget() == window->viewport(), "viewport is the central widget");
    if (fuse::core::timingBudgetsEnforced()) {
        expect(appInitMs + totalMs < 1000.0, fmt("editor initialises all panels + dockspace in < 1 s (%.1f ms)",
                                                 appInitMs + totalMs));
    } else {
        std::printf("SKIP 1 s budget (instrumented build) — measured %.1f ms\n", appInitMs + totalMs);
    }
    return g_failures == 0 ? 0 : 1;
}

// ---- default layout --------------------------------------------------------------------------------

int gateLayout() {
    MainWindow window(samplesRoot(), quietOptions());
    expect(showAndExpose(window), "main window exposed");

    struct Required {
        const char* name;
        QWidget* widget;
        Qt::DockWidgetArea area;
    };
    const Required required[] = {
        {MainWindow::kHierarchyDock, window.hierarchy(), Qt::LeftDockWidgetArea},
        {MainWindow::kInspectorDock, window.inspector(), Qt::RightDockWidgetArea},
        {MainWindow::kConsoleDock, window.console(), Qt::BottomDockWidgetArea},
    };
    auto check = [&](const char* phase) {
        QWidget* vp = window.viewport();
        expect(reallyVisible(vp), fmt("[%s] viewport visible (%dx%d)", phase, vp->width(), vp->height()));
        expect(vp->width() >= 320 && vp->height() >= 180, fmt("[%s] viewport keeps its minimum size", phase));
        const QRect vpRect(vp->mapTo(&window, QPoint(0, 0)), vp->size());
        for (const Required& r : required) {
            QDockWidget* dock = window.dock(r.name);
            const bool ok = dock != nullptr && !dock->isFloating() && reallyVisible(dock) && reallyVisible(r.widget) &&
                            window.dockWidgetArea(dock) == r.area;
            expect(ok, fmt("[%s] %s docked in its default area and visible (not behind a tab)", phase, r.name));
            if (dock != nullptr) {
                const QRect dr(dock->mapTo(&window, QPoint(0, 0)), dock->size());
                expect(!dr.intersects(vpRect), fmt("[%s] %s does not overlap the viewport", phase, r.name));
                expect(dr.width() >= 150 && dr.height() >= 100,
                       fmt("[%s] %s has a usable size (%dx%d)", phase, r.name, dr.width(), dr.height()));
            }
        }
        for (QDockWidget* dock : window.docks()) {
            expect(!dock->isHidden() && window.dockWidgetArea(dock) != Qt::NoDockWidgetArea,
                   fmt("[%s] %s present in the dockspace", phase, qPrintable(dock->objectName())));
        }
    };
    check("default");

    // Layout persistence: save, scramble (float + close), restore -> identical visible layout.
    const QByteArray saved = window.saveLayout();
    window.dock(MainWindow::kConsoleDock)->setFloating(true);
    window.dock(MainWindow::kHierarchyDock)->close();
    window.dock(MainWindow::kInspectorDock)->setFloating(true);
    QCoreApplication::processEvents();
    expect(window.restoreLayout(saved), "restoreLayout accepts the saved state");
    QCoreApplication::processEvents();
    check("restored");

    window.dock(MainWindow::kConsoleDock)->close();
    window.dock(MainWindow::kInspectorDock)->setFloating(true);
    window.resetToDefaultLayout();
    QCoreApplication::processEvents();
    check("reset");
    return g_failures == 0 ? 0 : 1;
}

// ---- dark theme --------------------------------------------------------------------------------------

QColor pixelAt(QWidget& w, QPoint logical) {
    const QImage img = w.grab().toImage();
    const qreal dpr = img.devicePixelRatio();
    const QPoint p(static_cast<int>(logical.x() * dpr), static_cast<int>(logical.y() * dpr));
    return img.pixelColor(std::clamp(p.x(), 0, img.width() - 1), std::clamp(p.y(), 0, img.height() - 1));
}

int colorDistance(const QColor& a, const QColor& b) {
    return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
}

/// Text ink colour: the most frequent non-background pixel (interior of large glyph stems, full
/// coverage — immune to antialiasing / subpixel fringes).
QColor inkColor(QWidget& w, const QColor& background) {
    const QImage img = w.grab().toImage();
    std::map<QRgb, int> counts;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (colorDistance(c, background) > 8) {
                ++counts[c.rgb()];
            }
        }
    }
    QRgb best = background.rgb();
    int bestN = 0;
    for (const auto& [rgb, n] : counts) {
        if (n > bestN) {
            bestN = n;
            best = rgb;
        }
    }
    return QColor(best);
}

/// Real pointer hover (xcb enter/move events) over `local` in `w`, then sample `sample`.
QColor hoverPixel(QWidget& w, QPoint local, QPoint sample) {
    QTest::mouseMove(&w, local);
    QTest::qWait(50);
    QCoreApplication::processEvents();
    return pixelAt(w, sample);
}

int gateTheme() {
    using fuse::editor::qt::kDarkThemeSpec;
    using fuse::editor::qt::themeColor;

    // 1. Table: every spec role present, colour == spec rounded to 8 bits.
    for (const auto& spec : kDarkThemeSpec) {
        const QColor c = themeColor(spec.role);
        const auto q = [](float v) { return static_cast<int>(v * 255.f + 0.5f); };
        expect(c.isValid() && c.red() == q(spec.r) && c.green() == q(spec.g) && c.blue() == q(spec.b) &&
                   c.alpha() == q(spec.a),
               fmt("spec %-16s = (%.2f, %.2f, %.2f, %.2f) -> %s", spec.role, spec.r, spec.g, spec.b, spec.a,
                   qPrintable(c.name(QColor::HexArgb))));
    }

    // 2. Applied: application palette and stylesheet carry every role.
    const QPalette pal = QApplication::palette();
    expect(pal.color(QPalette::Active, QPalette::Window) == themeColor("WindowBg"), "palette Window = WindowBg");
    expect(pal.color(QPalette::Active, QPalette::WindowText) == themeColor("Text"), "palette WindowText = Text");
    expect(pal.color(QPalette::Active, QPalette::Text) == themeColor("Text"), "palette Text = Text");
    expect(pal.color(QPalette::Disabled, QPalette::Text) == themeColor("TextDisabled"),
           "palette Disabled Text = TextDisabled");
    expect(pal.color(QPalette::Active, QPalette::Base) == themeColor("FrameBg"), "palette Base = FrameBg");
    expect(pal.color(QPalette::Active, QPalette::Button) == themeColor("Button"), "palette Button = Button");
    expect(pal.color(QPalette::Active, QPalette::ToolTipBase) == themeColor("PopupBg"), "palette ToolTipBase = PopupBg");
    expect(pal.color(QPalette::Active, QPalette::Highlight) == themeColor("SliderGrab"), "palette Highlight = accent");
    const QString sheet = qApp->styleSheet();
    expect(sheet == fuse::editor::qt::darkStyleSheet(), "editor stylesheet installed on the application");
    for (const auto& spec : kDarkThemeSpec) {
        const QColor c = themeColor(spec.role);
        const QString css = c.alpha() == 255
                                ? c.name(QColor::HexRgb)
                                : QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
        const bool inPalette = [&]() {
            for (int role = 0; role < QPalette::NColorRoles; ++role) {
                if (pal.color(QPalette::Active, static_cast<QPalette::ColorRole>(role)) == c) {
                    return true;
                }
            }
            return false;
        }();
        expect(sheet.contains(css) || inPalette, fmt("%s applied (stylesheet or palette)", spec.role));
    }

    // 3. Rendered: sample real widgets under the editor theme.
    QWidget sampler;
    sampler.setWindowTitle(QStringLiteral("theme sampler"));
    auto* layout = new QVBoxLayout(&sampler);
    layout->setContentsMargins(16, 16, 16, 16);
    auto* menuBar = new QMenuBar(&sampler);
    QMenu* menuBarMenu = menuBar->addMenu(QStringLiteral("File"));
    menuBarMenu->addAction(QStringLiteral("Quit"));
    layout->setMenuBar(menuBar);
    auto* edit = new QLineEdit(&sampler);
    auto* editHover = new QLineEdit(&sampler);
    auto* editFocus = new QLineEdit(&sampler);
    auto* button = new QPushButton(QStringLiteral("B"), &sampler);
    auto* buttonHover = new QPushButton(QStringLiteral("B"), &sampler);
    auto* buttonDown = new QPushButton(QStringLiteral("B"), &sampler);
    auto* tabs = new QTabBar(&sampler);
    tabs->addTab(QStringLiteral("A"));
    tabs->addTab(QStringLiteral("B"));
    tabs->addTab(QStringLiteral("C"));
    tabs->setCurrentIndex(0);
    auto* tree = new QTreeWidget(&sampler);
    tree->setHeaderLabels({QStringLiteral("H")});
    tree->setMinimumHeight(140);
    for (int i = 0; i < 3; ++i) {
        tree->addTopLevelItem(new QTreeWidgetItem(QStringList{QStringLiteral("i")}));
    }
    auto* slider = new QSlider(Qt::Horizontal, &sampler);
    auto* sliderDown = new QSlider(Qt::Horizontal, &sampler);
    auto* check = new QCheckBox(&sampler);
    check->setChecked(true);
    auto* splitter = new QSplitter(Qt::Horizontal, &sampler);
    splitter->addWidget(new QWidget(splitter));
    splitter->addWidget(new QWidget(splitter));
    splitter->setHandleWidth(6);
    splitter->setMinimumHeight(20);
    auto* text = new QLabel(QStringLiteral("IIII"), &sampler);
    auto* disabledText = new QLabel(QStringLiteral("IIII"), &sampler);
    QFont big = text->font();
    big.setPixelSize(48);
    big.setBold(true);
    text->setFont(big);
    disabledText->setFont(big);
    disabledText->setEnabled(false);
    auto* blank = new QWidget(&sampler);
    blank->setMinimumHeight(20);
    for (QWidget* w : std::initializer_list<QWidget*>{edit, editHover, editFocus, button, buttonHover, buttonDown, tabs,
                                                      tree, slider, sliderDown, check, splitter, text, disabledText,
                                                      blank}) {
        layout->addWidget(w);
    }
    QMainWindow dockHost;
    auto* dockA = new QDockWidget(QStringLiteral("T"), &dockHost);
    auto* dockB = new QDockWidget(QStringLiteral("T"), &dockHost);
    dockA->setWidget(new QWidget(dockA));
    dockB->setWidget(new QWidget(dockB));
    dockHost.addDockWidget(Qt::LeftDockWidgetArea, dockA);
    dockHost.addDockWidget(Qt::RightDockWidgetArea, dockB);
    dockHost.setCentralWidget(new QWidget(&dockHost));
    dockHost.resize(500, 200);
    sampler.move(0, 0);
    dockHost.move(600, 0); // no window manager under Xvfb: keep the windows from overlapping
    dockB->setProperty("fuseActive", true);

    sampler.resize(420, 760);
    expect(showAndExpose(sampler), "sampler exposed");
    expect(showAndExpose(dockHost), "dock host exposed");
    dockB->style()->unpolish(dockB);
    dockB->style()->polish(dockB);

    sampler.activateWindow();
    const bool active = QTest::qWaitForWindowActive(&sampler, 3000);
    editFocus->setFocus();
    editHover->setAttribute(Qt::WA_UnderMouse, true);
    buttonDown->setDown(true);
    tree->topLevelItem(1)->setSelected(true);
    QCoreApplication::processEvents();

    struct Probe {
        const char* role;
        QColor got;
        int tolerance;
    };
    std::vector<Probe> probes;
    auto mid = [](QWidget* w) { return QPoint(w->width() / 2, w->height() / 2); };
    probes.push_back({"WindowBg", pixelAt(*blank, mid(blank)), 0});
    probes.push_back({"MenuBarBg", pixelAt(*menuBar, QPoint(menuBar->width() - 4, menuBar->height() / 2)), 0});
    probes.push_back({"FrameBg", pixelAt(*edit, mid(edit)), 0});
    probes.push_back({"FrameBgHovered", pixelAt(*editHover, mid(editHover)), 0});
    if (active && editFocus->hasFocus()) {
        probes.push_back({"FrameBgActive", pixelAt(*editFocus, mid(editFocus)), 0});
    } else {
        std::printf("note: sampler window not active (no focus) — FrameBgActive verified via stylesheet only\n");
    }
    probes.push_back({"Button", pixelAt(*button, QPoint(4, button->height() / 2)), 0});
    QTest::mouseMove(&sampler, QPoint(2, 2)); // pointer into the window first (xcb enter)
    QTest::qWait(50);
    probes.push_back({"ButtonHovered", hoverPixel(*buttonHover, QPoint(buttonHover->width() / 2, buttonHover->height() / 2),
                                                  QPoint(4, buttonHover->height() / 2)),
                      0});
    probes.push_back({"ButtonActive", pixelAt(*buttonDown, QPoint(4, buttonDown->height() / 2)), 0});
    probes.push_back({"Border", pixelAt(*button, QPoint(button->width() / 2, 0)), 0});
    const QRect tabSel = tabs->tabRect(0);
    const QRect tabIdle = tabs->tabRect(2);
    probes.push_back({"TabActive", pixelAt(*tabs, QPoint(tabSel.left() + 2, tabSel.center().y())), 0});
    probes.push_back({"Tab", pixelAt(*tabs, QPoint(tabIdle.left() + 2, tabIdle.center().y())), 0});
    probes.push_back({"ChildBg", pixelAt(*tree->viewport(), QPoint(tree->viewport()->width() - 4,
                                                                   tree->viewport()->height() - 4)),
                      0});
    // Selected rows: Header; the selected current row of the focused view: HeaderActive.
    const QRect selRect = tree->visualItemRect(tree->topLevelItem(1));
    probes.push_back({"Header", pixelAt(*tree->viewport(), QPoint(tree->viewport()->width() - 4, selRect.center().y())), 0});
    tree->setCurrentItem(tree->topLevelItem(0), 0, QItemSelectionModel::Select);
    tree->setFocus();
    QCoreApplication::processEvents();
    if (active && tree->hasFocus()) {
        const QRect curRect = tree->visualItemRect(tree->topLevelItem(0));
        probes.push_back({"HeaderActive",
                          pixelAt(*tree->viewport(), QPoint(tree->viewport()->width() - 4, curRect.center().y())), 0});
    }
    // Hovered (non-selected) row: HeaderHovered.
    const QRect hovRect = tree->visualItemRect(tree->topLevelItem(2));
    probes.push_back({"HeaderHovered",
                      hoverPixel(*tree->viewport(), hovRect.center(),
                                 QPoint(tree->viewport()->width() - 4, hovRect.center().y())),
                      0});
    const QRect tabHov = tabs->tabRect(1);
    probes.push_back({"TabHovered", hoverPixel(*tabs, tabHov.center(), QPoint(tabHov.left() + 2, tabHov.center().y())), 0});
    QTest::mouseMove(&sampler, QPoint(2, 2));
    probes.push_back({"Header", pixelAt(*tree->header(), QPoint(tree->header()->width() - 4, tree->header()->height() / 2)),
                      0});
    probes.push_back({"SliderGrab", pixelAt(*slider, QPoint(6, slider->height() / 2)), 0});
    QTest::mousePress(sliderDown, Qt::LeftButton, Qt::NoModifier, QPoint(6, sliderDown->height() / 2));
    QCoreApplication::processEvents();
    probes.push_back({"SliderGrabActive", pixelAt(*sliderDown, QPoint(6, sliderDown->height() / 2)), 0});
    QTest::mouseRelease(sliderDown, Qt::LeftButton, Qt::NoModifier, QPoint(6, sliderDown->height() / 2));
    probes.push_back({"CheckMark", pixelAt(*check, QPoint(6, check->height() / 2)), 0});
    QWidget* handle = splitter->handle(1);
    probes.push_back({"Separator", pixelAt(*handle, mid(handle)), 0});
    probes.push_back({"Text", inkColor(*text, themeColor("WindowBg")), 0});
    probes.push_back({"TextDisabled", inkColor(*disabledText, themeColor("WindowBg")), 0});
    probes.push_back({"TitleBg", pixelAt(*dockA, QPoint(dockA->width() / 2, 3)), 0});
    probes.push_back({"TitleBgActive", pixelAt(*dockB, QPoint(dockB->width() / 2, 3)), 0});

    QMenu popup;
    popup.addAction(QStringLiteral("x"));
    popup.popup(sampler.mapToGlobal(QPoint(40, 40)));
    (void)QTest::qWaitForWindowExposed(&popup, 3000);
    // PopupBg has alpha 0.98: composited on an opaque surface it may differ by <= 0.02 * 255.
    probes.push_back({"PopupBg", pixelAt(popup, QPoint(popup.width() / 2, 1)), 6});
    popup.close();

    // DockingPreview (alpha 0.7): the QRubberBand QMainWindow shows while a dock is dragged,
    // composited over the window background -> 0.7 * DockingPreview + 0.3 * WindowBg.
    {
        QRubberBand band(QRubberBand::Rectangle, blank);
        band.setGeometry(QRect(QPoint(0, 0), blank->size()));
        band.show();
        QCoreApplication::processEvents();
        const QColor got = pixelAt(*blank, mid(blank));
        const QColor dp = themeColor("DockingPreview");
        const QColor bg = themeColor("WindowBg");
        const float a = static_cast<float>(dp.alpha()) / 255.f;
        const QColor want(static_cast<int>(std::lround(dp.red() * a + bg.red() * (1.f - a))),
                          static_cast<int>(std::lround(dp.green() * a + bg.green() * (1.f - a))),
                          static_cast<int>(std::lround(dp.blue() * a + bg.blue() * (1.f - a))));
        probes.push_back({"DockingPreview", got, 0});
        const int d = colorDistance(got, want);
        expect(d <= 2, fmt("rendered DockingPreview rubber band %s == spec composited over WindowBg %s (diff %d)",
                           qPrintable(got.name()), qPrintable(want.name()), d));
    }

    std::set<std::string> pixelVerified;
    for (const Probe& p : probes) {
        pixelVerified.insert(p.role);
        if (std::string(p.role) == "DockingPreview") {
            continue; // checked above (translucent)
        }
        const QColor want = themeColor(p.role);
        const int d = colorDistance(p.got, want);
        // +-1 per channel: 8-bit rounding of the float spec may differ by one step in the raster path.
        expect(d <= std::max(p.tolerance, 1), fmt("rendered %-16s %s == spec %s (max channel diff %d)", p.role,
                                     qPrintable(p.got.name()), qPrintable(want.name()), d));
        pixelVerified.insert(p.role);
    }
    std::printf("pixel-verified %zu / %zu spec colours; rule-verified only:", pixelVerified.size(), kDarkThemeSpec.size());
    for (const auto& spec : kDarkThemeSpec) {
        if (!pixelVerified.count(spec.role)) {
            std::printf(" %s", spec.role);
        }
    }
    std::printf("\n");
    return g_failures == 0 ? 0 : 1;
}

// ---- font / DPI ------------------------------------------------------------------------------------

/// Ink bounding box (physical px) of dark-on-light rendering of `text` in `font` at `dpr`.
QRect inkBox(const QFont& font, const QString& text, qreal dpr) {
    QFontMetricsF fm(font);
    const QSizeF logical(fm.horizontalAdvance(text) + 20, fm.height() + 20);
    QImage img(static_cast<int>(std::ceil(logical.width() * dpr)), static_cast<int>(std::ceil(logical.height() * dpr)),
               QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::white);
    {
        QPainter p(&img);
        p.setFont(font);
        p.setPen(Qt::black);
        p.drawText(QPointF(10, 10 + fm.ascent()), text);
    }
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qGray(img.pixel(x, y)) < 128) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    return maxX < 0 ? QRect() : QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

void collectTexts(QObject* root, QSet<QChar>& chars, int& strings) {
    auto add = [&](const QString& s) {
        ++strings;
        for (QChar c : s) {
            if (c != QLatin1Char('&') && c.isPrint()) {
                chars.insert(c);
            }
        }
    };
    const QList<QObject*> objects = root->findChildren<QObject*>();
    for (QObject* o : objects) {
        if (auto* w = qobject_cast<QWidget*>(o)) {
            add(w->windowTitle());
            add(w->toolTip());
        }
        if (auto* l = qobject_cast<QLabel*>(o)) {
            add(l->text());
        } else if (auto* b = qobject_cast<QAbstractButton*>(o)) {
            add(b->text());
        } else if (auto* e = qobject_cast<QLineEdit*>(o)) {
            add(e->text());
            add(e->placeholderText());
        } else if (auto* a = qobject_cast<QAction*>(o)) {
            add(a->text());
        } else if (auto* t = qobject_cast<QTreeWidget*>(o)) {
            for (QTreeWidgetItemIterator it(t); *it != nullptr; ++it) {
                for (int c = 0; c < t->columnCount(); ++c) {
                    add((*it)->text(c));
                }
            }
            if (t->headerItem() != nullptr) {
                for (int c = 0; c < t->columnCount(); ++c) {
                    add(t->headerItem()->text(c));
                }
            }
        } else if (auto* list = qobject_cast<QListWidget*>(o)) {
            for (int i = 0; i < list->count(); ++i) {
                add(list->item(i)->text());
            }
        } else if (auto* tb = qobject_cast<QTabBar*>(o)) {
            for (int i = 0; i < tb->count(); ++i) {
                add(tb->tabText(i));
            }
        } else if (auto* pt = qobject_cast<QPlainTextEdit*>(o)) {
            add(pt->toPlainText());
            add(pt->placeholderText());
        }
    }
}

int gateFont(double expectedScale) {
    const QString family = fuse::editor::qt::loadEditorFont();
    expect(!family.isEmpty(), fmt("bundled editor font registered from Qt resource (family '%s')", qPrintable(family)));
    expect(family == QStringLiteral("Open Sans"), "editor font family is Open Sans");
    const QFont appFont = QApplication::font();
    const QFontInfo info(appFont);
    expect(info.family() == family, fmt("application font resolves to the bundled family (got '%s')",
                                        qPrintable(info.family())));
    expect(appFont.pixelSize() == fuse::editor::qt::kEditorFontPixelSize,
           fmt("application font pixel size %d", appFont.pixelSize()));

    MainWindow window(samplesRoot(), quietOptions());
    expect(showAndExpose(window), "main window exposed");
    const qreal dpr = window.devicePixelRatioF();
    std::printf("QT_SCALE_FACTOR=%s screen dpr=%.3f window dpr=%.3f logicalDpi=%.1f\n",
                qgetenv("QT_SCALE_FACTOR").constData(), QGuiApplication::primaryScreen()->devicePixelRatio(), dpr,
                QGuiApplication::primaryScreen()->logicalDotsPerInch());
    expect(std::abs(dpr - expectedScale) < 1e-3, fmt("device pixel ratio follows QT_SCALE_FACTOR (%.3f)", expectedScale));

    // Logical metrics are DPI independent (layout is in logical px) ...
    const QFontMetricsF fm(appFont);
    const double capLogical = fm.capHeight();
    std::printf("logical: height %.3f ascent %.3f capHeight %.3f advance('HHHHHHHHHH') %.3f\n", fm.height(),
                fm.ascent(), capLogical, fm.horizontalAdvance(QStringLiteral("HHHHHHHHHH")));
    expect(std::abs(fm.height() - 18.0) < 1.5, fmt("logical line height ~18 px at 13 px Open Sans (%.2f)", fm.height()));
    // ... while rendered glyphs scale with the device pixel ratio.
    const QRect capBox = inkBox(appFont, QStringLiteral("H"), dpr);
    const double capPhysical = capBox.height();
    const double wantCap = capLogical * dpr;
    expect(std::abs(capPhysical - wantCap) <= 1.5,
           fmt("rendered cap height %.0f physical px == capHeight x dpr %.2f (+-1.5)", capPhysical, wantCap));
    const QRect runBox = inkBox(appFont, QStringLiteral("HHHHHHHHHH"), dpr);
    const QRect oneBox = inkBox(appFont, QStringLiteral("H"), dpr);
    const double pitch = static_cast<double>(runBox.width() - oneBox.width()) / 9.0;
    const double wantPitch = fm.horizontalAdvance(QLatin1Char('H')) * dpr;
    expect(std::abs(pitch - wantPitch) <= 0.35,
           fmt("rendered glyph advance %.3f physical px == advance x dpr %.3f", pitch, wantPitch));
    // A real widget: the rendered label backing store is dpr x its logical size.
    QPixmap labelGrab = window.statusBar()->grab();
    expect(std::abs(labelGrab.width() - window.statusBar()->width() * dpr) <= 1.0,
           fmt("widgets render at dpr resolution (%d px for %d logical)", labelGrab.width(), window.statusBar()->width()));

    // No missing glyphs: printable ASCII + Latin-1, the viewport overlay symbols and every string
    // shown anywhere in the editor window (titles, labels, buttons, menus, trees, lists, tabs).
    QSet<QChar> chars;
    for (char16_t c = 0x20; c < 0x7f; ++c) {
        chars.insert(QChar(c));
    }
    for (char16_t c = 0xa1; c <= 0xff; ++c) {
        chars.insert(QChar(c));
    }
    for (QChar c : QStringLiteral("×—°…α")) {
        chars.insert(c);
    }
    int strings = 0;
    collectTexts(&window, chars, strings);
    const QRawFont raw = QRawFont::fromFont(appFont);
    expect(raw.isValid(), "raw font valid");
    QString missing;
    for (QChar c : chars) {
        if (!raw.supportsCharacter(c)) {
            missing += c;
        }
    }
    std::printf("glyph coverage: %lld distinct characters from %d UI strings + ASCII/Latin-1\n",
                static_cast<long long>(chars.size()), strings);
    expect(missing.isEmpty(), fmt("no missing glyphs (missing: '%s')", qPrintable(missing)));
    // Rendered: each glyph produces ink (not an empty box / fallback-less blank).
    int blank = 0;
    for (QChar c : chars) {
        if (!c.isSpace() && c != QChar(0xa0) && c != QChar(0xad) && inkBox(appFont, QString(c), dpr).isNull()) {
            ++blank;
        }
    }
    expect(blank == 0, fmt("every non-space glyph renders ink at dpr %.2f (%d blank)", dpr, blank));
    return g_failures == 0 ? 0 : 1;
}

// ---- WASD ------------------------------------------------------------------------------------------

void sendKey(QWidget* w, QEvent::Type type, int key, bool autoRepeat = false) {
    QKeyEvent e(type, key, Qt::NoModifier, QString(), autoRepeat);
    QApplication::sendEvent(w, &e);
}

int gateWasd() {
    MainWindow window(samplesRoot(), quietOptions());
    expect(showAndExpose(window), "main window exposed");
    ViewportPlaceholderWidget* vp = window.viewport();
    fuse::editor::ViewportPanel& panel = vp->panel();
    const float speed = panel.camera().moveSpeed;
    constexpr float kDt = 1.f / 60.f;
    auto pos = [&]() { return panel.position(); };
    auto dist = [](const fuse::ecs::vec3& a, const fuse::ecs::vec3& b) {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    };

    QTest::mousePress(vp, Qt::RightButton, Qt::NoModifier, vp->rect().center());
    vp->advanceFrame(kDt);
    auto p0 = pos();
    vp->advanceFrame(kDt);
    expect(dist(pos(), p0) == 0.f, "no key held: camera does not drift");

    // Same-frame response: the key event lands, the very next frame moves by exactly speed * dt.
    QTest::keyPress(vp, Qt::Key_W);
    p0 = pos();
    vp->advanceFrame(kDt);
    const float first = dist(pos(), p0);
    expect(std::abs(first - speed * kDt) < 1e-4f,
           fmt("first frame after W press moves speed*dt (%.6f vs %.6f)", first, speed * kDt));
    const fuse::ecs::vec3 fwd = panel.forward();
    const fuse::ecs::vec3 dir{(pos().x - p0.x) / first, (pos().y - p0.y) / first, (pos().z - p0.z) / first, 0.f};
    expect(dist(dir, fwd) < 1e-4f, "W moves along the camera forward axis");

    // Constant motion per frame over 2 s of 60 fps frames, with X11-style auto-repeat bursts.
    std::vector<double> steps;
    for (int f = 0; f < 120; ++f) {
        if (f % 3 == 1) {
            sendKey(vp, QEvent::KeyRelease, Qt::Key_W, true);
            sendKey(vp, QEvent::KeyPress, Qt::Key_W, true);
        }
        const auto before = pos();
        vp->advanceFrame(kDt);
        steps.push_back(dist(pos(), before));
    }
    const auto [mn, mx] = std::minmax_element(steps.begin(), steps.end());
    std::printf("W held 120 frames @60fps: step min %.7f max %.7f (want %.7f)\n", *mn, *mx, speed * kDt);
    expect(*mx - *mn < 1e-4 && std::abs(*mn - speed * kDt) < 1e-4,
           "motion per frame constant (no jitter) through auto-repeat events");

    // Release: the next frame stops (no lag, no drift).
    QTest::keyRelease(vp, Qt::Key_W);
    p0 = pos();
    vp->advanceFrame(kDt);
    expect(dist(pos(), p0) == 0.f, "W release stops the camera on the next frame");

    // Diagonal is not faster; Shift multiplies; A/D/Q/E map to the camera basis.
    QTest::keyPress(vp, Qt::Key_W);
    QTest::keyPress(vp, Qt::Key_D);
    p0 = pos();
    vp->advanceFrame(kDt);
    expect(std::abs(dist(pos(), p0) - speed * kDt) < 1e-4f, "W+D diagonal moves speed*dt (normalised)");
    QTest::keyRelease(vp, Qt::Key_D);
    QTest::keyPress(vp, Qt::Key_Shift);
    p0 = pos();
    vp->advanceFrame(kDt);
    expect(std::abs(dist(pos(), p0) - speed * panel.camera().fastMultiplier * kDt) < 1e-3f,
           "Shift applies the fast multiplier");
    QTest::keyRelease(vp, Qt::Key_Shift);
    QTest::keyRelease(vp, Qt::Key_W);
    struct KeyAxis {
        int key;
        const char* name;
        fuse::ecs::vec3 axis;
    };
    const fuse::ecs::vec3 r = panel.right();
    const KeyAxis axes[] = {{Qt::Key_S, "S", {-fwd.x, -fwd.y, -fwd.z, 0.f}},
                            {Qt::Key_A, "A", {-r.x, -r.y, -r.z, 0.f}},
                            {Qt::Key_D, "D", {r.x, r.y, r.z, 0.f}},
                            {Qt::Key_E, "E", {0.f, 1.f, 0.f, 0.f}},
                            {Qt::Key_Q, "Q", {0.f, -1.f, 0.f, 0.f}}};
    for (const KeyAxis& a : axes) {
        QTest::keyPress(vp, a.key);
        p0 = pos();
        vp->advanceFrame(kDt);
        const float d = dist(pos(), p0);
        const fuse::ecs::vec3 moved{(pos().x - p0.x) / d, (pos().y - p0.y) / d, (pos().z - p0.z) / d, 0.f};
        expect(std::abs(d - speed * kDt) < 1e-4f && dist(moved, a.axis) < 1e-3f,
               fmt("%s moves speed*dt along its axis", a.name));
        QTest::keyRelease(vp, a.key);
    }

    // Focus loss never leaves the camera flying.
    QTest::keyPress(vp, Qt::Key_W);
    window.hierarchy()->tree()->setFocus();
    QCoreApplication::processEvents();
    p0 = pos();
    vp->advanceFrame(kDt);
    if (!vp->hasFocus() && QApplication::focusWidget() != nullptr) {
        expect(dist(pos(), p0) == 0.f, "keys are released when the viewport loses focus");
    }
    QTest::mouseRelease(vp, Qt::RightButton, Qt::NoModifier, vp->rect().center());

    // Real 60 Hz frame pump for 1 s with W held: velocity (step / dt) is exactly moveSpeed every
    // frame whatever the timer jitter, and frames arrive at ~60 fps.
    vp->setFocus();
    QTest::mousePress(vp, Qt::RightButton, Qt::NoModifier, vp->rect().center());
    QTest::keyPress(vp, Qt::Key_W);
    std::vector<double> dts;
    std::vector<double> velocities;
    fuse::ecs::vec3 last = pos();
    QObject::connect(vp, &ViewportPlaceholderWidget::frameAdvanced, vp, [&](float dt) {
        const fuse::ecs::vec3 now = pos();
        if (dt > 0.f) {
            dts.push_back(dt);
            velocities.push_back(dist(now, last) / dt);
        }
        last = now;
    });
    vp->setFramePumpEnabled(true);
    QElapsedTimer wall;
    wall.start();
    while (wall.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    vp->setFramePumpEnabled(false);
    QTest::keyRelease(vp, Qt::Key_W);
    QTest::mouseRelease(vp, Qt::RightButton, Qt::NoModifier, vp->rect().center());
    const auto [vmin, vmax] = std::minmax_element(velocities.begin(), velocities.end());
    const double medDt = percentile(dts, 0.5) * 1e3;
    std::printf("frame pump 1 s: %zu frames, dt median %.2f ms p95 %.2f ms max %.2f ms; velocity min %.4f max %.4f "
                "(moveSpeed %.4f)\n",
                dts.size(), medDt, percentile(dts, 0.95) * 1e3, percentile(dts, 1.0) * 1e3, *vmin, *vmax, speed);
    expect(dts.size() >= 30u, "frame pump delivered frames");
    expect(std::abs(*vmin - speed) < 1e-2 * speed && std::abs(*vmax - speed) < 1e-2 * speed,
           "camera velocity == moveSpeed on every pumped frame (motion follows measured dt: no jitter)");
    if (fuse::core::timingBudgetsEnforced()) {
        expect(medDt > 14.0 && medDt < 20.0, fmt("frame pump runs at ~60 fps (median dt %.2f ms)", medDt));
    }
    return g_failures == 0 ? 0 : 1;
}

// ---- context menu ----------------------------------------------------------------------------------

int gateContextMenu() {
    MainWindow window(samplesRoot(), quietOptions());
    expect(showAndExpose(window), "main window exposed");
    ViewportPlaceholderWidget* vp = window.viewport();
    const qreal dpr = vp->devicePixelRatioF();
    std::printf("dpr %.3f, window %dx%d, viewport at (%d,%d) %dx%d\n", dpr, window.width(), window.height(),
                vp->mapTo(&window, QPoint(0, 0)).x(), vp->mapTo(&window, QPoint(0, 0)).y(), vp->width(), vp->height());

    bool metricsMatched = true;
    int bothFlipped = 0;
    // Expected flips follow from the geometry alone: open to the left / above when the menu would
    // cross the right / bottom window edge.
    auto clickAt = [&](const char* name, QPoint local) {
        QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, local);
        QMenu* menu = vp->activeContextMenu();
        expect(menu != nullptr, fmt("[%s] right-click opens the context menu", name));
        if (menu == nullptr) {
            return;
        }
        (void)QTest::qWaitForWindowExposed(menu, 3000);
        QCoreApplication::processEvents();
        const fuse::editor::ContextMenuPlacement& p = vp->contextMenuModel().placement();
        const QPoint inWindow = vp->mapTo(&window, local);
        expect(std::abs(p.clickX - inWindow.x()) < 0.51f && std::abs(p.clickY - inWindow.y()) < 0.51f,
               fmt("[%s] placement click (%.1f, %.1f) == click in window (%d, %d) through DPR %.2f", name, p.clickX,
                   p.clickY, inWindow.x(), inWindow.y(), dpr));
        const QRect want = vp->placementGlobalRect();
        const QRect got = menu->geometry();
        expect(got == want, fmt("[%s] QMenu geometry (%d,%d %dx%d) == placement (%d,%d %dx%d)", name, got.x(),
                                got.y(), got.width(), got.height(), want.x(), want.y(), want.width(), want.height()));
        const float modelH = vp->contextMenuModel().menuHeight();
        const float modelW = vp->contextMenuModel().menuWidth();
        const bool matched = std::lround(modelW) == got.width() && std::lround(modelH) == got.height();
        metricsMatched = metricsMatched && matched;
        const bool wantFlipX = inWindow.x() + got.width() > window.width();
        const bool wantFlipY = inWindow.y() + got.height() > window.height();
        std::printf("[%s] click window (%d,%d) model metrics %.0fx%.0f vs QMenu %dx%d%s; flipped x=%d y=%d\n", name,
                    inWindow.x(), inWindow.y(), modelW, modelH, got.width(), got.height(),
                    matched ? "" : " (re-placed with real size)", p.flippedX, p.flippedY);
        expect(p.flippedX == wantFlipX && p.flippedY == wantFlipY,
               fmt("[%s] edge flip x=%d y=%d as the window edges require", name, wantFlipX, wantFlipY));
        bothFlipped += (p.flippedX && p.flippedY) ? 1 : 0;
        const QRect windowRect(window.mapToGlobal(QPoint(0, 0)), window.size());
        expect(windowRect.contains(got), fmt("[%s] menu stays inside the editor window", name));
        menu->close();
        QCoreApplication::processEvents();
    };
    clickAt("centre", vp->rect().center());
    clickAt("top-left", QPoint(2, 2));
    clickAt("bottom-right", QPoint(vp->width() - 3, vp->height() - 3));
    clickAt("bottom-left", QPoint(2, vp->height() - 3));
    // Let the viewport reach the window's right / bottom edges (close the right + bottom docks).
    for (const char* name : {MainWindow::kInspectorDock, MainWindow::kMaterialEditorDock, MainWindow::kSdfSculptDock,
                             MainWindow::kConsoleDock, MainWindow::kProfilerDock, MainWindow::kSequencerDock}) {
        window.dock(name)->close();
    }
    window.statusBar()->hide();
    QCoreApplication::processEvents();
    clickAt("edge bottom-right", QPoint(vp->width() - 3, vp->height() - 3));
    clickAt("edge right-middle", QPoint(vp->width() - 3, vp->height() / 2));
    expect(bothFlipped >= 1, "a click at the window corner flips the menu left and up");
    expect(metricsMatched, "QMenu metrics equal the headless EntityContextMenu model (200 x items*22+seps*7+8)");

    // Drag with RMB = fly, not a context menu.
    QTest::mousePress(vp, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
    QMouseEvent move(QEvent::MouseMove, QPointF(140, 100), vp->mapToGlobal(QPointF(140, 100)), Qt::NoButton,
                     Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &move);
    QTest::mouseRelease(vp, Qt::RightButton, Qt::NoModifier, QPoint(140, 100));
    expect(vp->activeContextMenu() == nullptr, "RMB drag (fly) does not open the context menu");
    return g_failures == 0 ? 0 : 1;
}

// ---- B6.1 on FUSE APIs -----------------------------------------------------------------------------

QAction* findAction(QMenu* menu, const QString& text) {
    for (QAction* a : menu->actions()) {
        if (a->text() == text) {
            return a;
        }
    }
    return nullptr;
}

int gateFuseApi() {
    MainWindow window(samplesRoot(), quietOptions());
    expect(showAndExpose(window), "main window exposed");
    fuse::editor::EditorHost& host = window.host();
    ViewportPlaceholderWidget* vp = window.viewport();
    auto& registry = host.editorScene().registry();
    const int rows0 = window.hierarchy()->entityRowCount();
    const auto undo0 = host.undoStack().undoCount();

    // Viewport context menu -> EntityContextMenu::activate -> UndoStack -> ECS registry.
    QMenu* menu = vp->openContextMenuAt(vp->rect().center());
    (void)QTest::qWaitForWindowExposed(menu, 3000);
    QAction* sphere = findAction(menu, QStringLiteral("Create SDF Sphere"));
    expect(sphere != nullptr, "context menu lists the FUSE EntityContextMenu items (Create SDF Sphere)");
    if (sphere == nullptr) {
        return 1;
    }
    sphere->trigger();
    QCoreApplication::processEvents();
    const fuse::ecs::EntityID created = vp->contextMenuModel().lastCreated();
    expect(created.valid() && registry.alive(created), "create action spawned an ECS entity via EntityContextMenu");
    expect(host.undoStack().undoCount() == undo0 + 1, "create is one undo step on the host UndoStack");
    expect(window.hierarchy()->entityRowCount() == rows0 + 1, "hierarchy dock shows the new entity");
    expect(host.editorState().primarySelection == created, "new entity becomes the EditorState selection");
    // Inspector: PropertyInspector sections for the selection (Transform + SDFObject).
    window.refreshPanels();
    QTreeWidget* sections = window.inspector()->sectionTree();
    QStringList names;
    for (int i = 0; i < sections->topLevelItemCount(); ++i) {
        names << sections->topLevelItem(i)->text(0);
    }
    std::printf("inspector sections: %s\n", qPrintable(names.join(QStringLiteral(", "))));
    expect(sections->topLevelItemCount() >= 2, "inspector lists the selection's ECS component sections");

    // Hierarchy selection -> EditorState.
    host.editorState().selectedEntities.clear();
    host.editorState().primarySelection = fuse::ecs::EntityID::null();
    window.hierarchy()->refresh();
    QTreeWidgetItemIterator it(window.hierarchy()->tree());
    QTreeWidgetItem* row = nullptr;
    for (; *it != nullptr; ++it) {
        if ((*it)->text(0).endsWith(QStringLiteral("#%1").arg(created.index))) {
            row = *it;
        }
    }
    expect(row != nullptr, "hierarchy row for the entity");
    if (row != nullptr) {
        window.hierarchy()->tree()->setCurrentItem(row);
        expect(host.editorState().primarySelection == created, "selecting the hierarchy row selects via EditorState");
    }

    // Edit > Undo -> UndoStack::undo -> entity gone everywhere.
    QAction* undo = nullptr;
    for (QAction* a : window.menuBar()->actions()) {
        if (a->menu() != nullptr) {
            if (QAction* u = findAction(a->menu(), QStringLiteral("&Undo"))) {
                undo = u;
            }
        }
    }
    expect(undo != nullptr, "Edit > Undo present");
    if (undo != nullptr) {
        undo->trigger();
        QCoreApplication::processEvents();
        expect(!registry.alive(created), "Edit > Undo reverts the create through the UndoStack");
        expect(window.hierarchy()->entityRowCount() == rows0, "hierarchy follows the undo");
    }

    // Console: ConsolePanel command dispatch + FUSE log sink.
    fuse::editor::qt::ConsoleWidget* console = window.console();
    console->input()->setText(QStringLiteral("help"));
    QTest::keyClick(console->input(), Qt::Key_Return);
    expect(console->panel().lastExecutedCommand() == "help", "console line runs through ConsolePanel::executeCommand");
    fuse::log::info("qt-gate log line %d", 42);
    console->drainLog();
    expect(console->logView()->toPlainText().contains(QStringLiteral("qt-gate log line 42")),
           "FUSE logger output reaches the console dock");

    // PIE: Play button -> FeaturePaneBridge -> CommandQueue -> game tick -> EditorState.playing.
    QPushButton* play = nullptr;
    for (QPushButton* b : window.inspector()->findChildren<QPushButton*>()) {
        if (b->text().startsWith(QStringLiteral("Play"))) {
            play = b;
        }
    }
    expect(play != nullptr, "inspector Play button");
    if (play != nullptr) {
        play->click();
        {
            std::lock_guard<std::mutex> lock(window.sceneMutex());
            host.gameTick();
        }
        expect(host.editorState().playing, "Play posts through the command queue and the game tick enters PIE");
    }
    return g_failures == 0 ? 0 : 1;
}

// ---- UI frame time / idle frames / memory ----------------------------------------------------------

void populateScene(MainWindow& window, int count) {
    ViewportPlaceholderWidget* vp = window.viewport();
    for (int i = 0; i < count; ++i) {
        QMenu* menu = vp->openContextMenuAt(QPoint(20 + (i * 7) % std::max(1, vp->width() - 40), 20 + (i * 13) % std::max(1, vp->height() - 40)));
        QAction* a = findAction(menu, i % 2 == 0 ? QStringLiteral("Create SDF Sphere") : QStringLiteral("Create Cube"));
        if (a != nullptr) {
            a->trigger();
        }
        menu->close();
    }
    QCoreApplication::processEvents();
}

int gateUiFrame() {
    MainWindow window(samplesRoot(), quietOptions());
    expect(showAndExpose(window), "main window exposed");
    populateScene(window, 64);
    window.refreshPanels();
    for (int i = 0; i < 30; ++i) {
        window.measureUiFrame();
    }
    if (qEnvironmentVariableIsSet("FUSE_QT_UI_FRAME_BREAKDOWN")) { // diagnostics only
        auto timeRender = [](QWidget* w) {
            QImage img(w->size() * w->devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(w->devicePixelRatioF());
            QElapsedTimer t;
            t.start();
            for (int i = 0; i < 50; ++i) {
                w->render(&img);
            }
            return static_cast<double>(t.nsecsElapsed()) * 1e-6 / 50.0;
        };
        std::printf("render-to-image: window %.3f ms, viewport %.3f ms, menubar %.3f, statusbar %.3f\n",
                    timeRender(&window), timeRender(window.viewport()), timeRender(window.menuBar()),
                    timeRender(window.statusBar()));
        for (QDockWidget* d : window.docks()) {
            std::printf("  %-22s %.3f ms (visible %d)\n", qPrintable(d->objectName()), timeRender(d), d->isVisible());
        }
        {
            QImage img(window.size() * window.devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
            QElapsedTimer t;
            t.start();
            for (int i = 0; i < 50; ++i) {
                window.render(&img, QPoint(), QRegion(), QWidget::DrawWindowBackground);
            }
            std::printf("  main window self (no children) %.3f ms\n", static_cast<double>(t.nsecsElapsed()) * 1e-6 / 50.0);
        }
        const QString sheet = qApp->styleSheet();
        qApp->setStyleSheet(QString());
        QCoreApplication::processEvents();
        std::printf("  window without stylesheet %.3f ms\n", timeRender(&window));
        qApp->setStyleSheet(sheet);
        QCoreApplication::processEvents();
    }
    // (a) Steady-state editor frame: the camera is flying (RMB + W held), so every frame the viewport
    //     is invalidated and the status bar ticks; Qt repaints exactly the damaged widgets and flushes
    //     them to the window system. Timed end to end: input sample + update + paint + flush.
    ViewportPlaceholderWidget* vp = window.viewport();
    vp->setFocus();
    QTest::mousePress(vp, Qt::RightButton, Qt::NoModifier, vp->rect().center());
    QTest::keyPress(vp, Qt::Key_W);
    for (int i = 0; i < 30; ++i) {
        vp->advanceFrame(1.f / 60.f);
        QCoreApplication::processEvents();
    }
    std::vector<double> frameMs;
    for (int i = 0; i < 600; ++i) {
        QElapsedTimer t;
        t.start();
        vp->advanceFrame(1.f / 60.f);
        window.statusBar()->showMessage(QStringLiteral("frame %1").arg(i));
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        frameMs.push_back(static_cast<double>(t.nsecsElapsed()) * 1e-6);
    }
    QTest::keyRelease(vp, Qt::Key_W);
    QTest::mouseRelease(vp, Qt::RightButton, Qt::NoModifier, vp->rect().center());
    const double frameMed = percentile(frameMs, 0.5);
    std::printf("steady-state editor UI frame (camera flying; viewport %dx%d + status bar repaint + flush): median "
                "%.3f ms, p95 %.3f ms, max %.3f ms\n",
                vp->width(), vp->height(), frameMed, percentile(frameMs, 0.95), percentile(frameMs, 1.0));

    // (b) Worst case, informational: synchronous repaint of the entire window (every dock, e.g.
    //     after a resize / theme change) — what an immediate-mode UI would pay every frame.
    std::vector<double> ms;
    for (int i = 0; i < 300; ++i) {
        vp->advanceFrame(1.f / 60.f);
        ms.push_back(window.measureUiFrame());
    }
    const double med = percentile(ms, 0.5);
    const double p95 = percentile(ms, 0.95);
    std::printf("full-window repaint (%dx%d @dpr %.2f, 9 docks, %d hierarchy rows): median %.3f ms, p95 %.3f ms, "
                "max %.3f ms (%s build) [informational]\n",
                window.width(), window.height(), window.devicePixelRatioF(), window.hierarchy()->entityRowCount(), med,
                p95, percentile(ms, 1.0), releaseBuild() ? "Release" : "Debug");
    expect(window.profiler()->panel().frameCount() > 0u &&
               std::abs(window.profiler()->panel().latestFrame().cpuMs - static_cast<float>(ms.back())) < 1e-3f,
           "UI frame times feed the Profiler dock");
    if (!releaseBuild() || !fuse::core::timingBudgetsEnforced()) {
        std::printf("SKIP 2 ms budget: enforced in Release (non-instrumented) builds only\n");
        return g_failures == 0 ? 77 : 1;
    }
    expect(frameMed < 2.0, fmt("editor UI render time < 2 ms per steady-state frame (median %.3f ms)", frameMed));
    return g_failures == 0 ? 0 : 1;
}

int gateIdleFrames() {
    const int kFrames = qEnvironmentVariableIsSet("FUSE_QT_IDLE_DEBUG_FRAMES") ? qEnvironmentVariableIntValue("FUSE_QT_IDLE_DEBUG_FRAMES") : 10000;
    const int kMinRuns = qEnvironmentVariableIsSet("FUSE_QT_IDLE_DEBUG_FRAMES") ? 1 : 3;
    const int kMaxRuns = qEnvironmentVariableIsSet("FUSE_QT_IDLE_DEBUG_FRAMES") ? 1 : 6;
    constexpr double kSpikeFactor = 3.0;
    const long long rss0 = residentBytes();
    const long long heap0 = heapInUse();

    // One idle editor frame: viewport frame (no input), status/panel refresh, full window repaint
    // (the UI is re-rendered every frame: the worst case for a non-interactive frame).
    // Renderer share of the process: the first game ticks consume the viewport's Vulkan surface
    // hand-off and bring up the embedded runtime renderer (RhiContext / hybrid on Lavapipe); that
    // is the runtime, not the editor layer, and is measured separately (first run only).
    long long rendererRss = -1;
    auto runOnce = [&](std::vector<long long>& cpu, long long& heapGrowth, long long& rssAfter) {
        MainWindow window(samplesRoot(), quietOptions());
        showAndExpose(window);
        populateScene(window, 16);
        const long long rssBeforeRenderer = residentBytes();
        for (int i = 0; i < 60; ++i) {
            {
                std::lock_guard<std::mutex> lock(window.sceneMutex());
                window.host().gameTick();
            }
            if (i == 4 && rendererRss < 0) {
                rendererRss = residentBytes() - rssBeforeRenderer;
            }
        }
        for (int i = 0; i < 60; ++i) {
            window.viewport()->advanceFrame(1.f / 60.f);
            {
                std::lock_guard<std::mutex> lock(window.sceneMutex());
                window.host().gameTick();
            }
            window.repaint();
            QCoreApplication::processEvents();
        }
        const long long heapStart = heapInUse();
        const QByteArray skip = qgetenv("FUSE_QT_IDLE_DEBUG_SKIP"); // diagnostics only
        for (int f = 0; f < kFrames; ++f) {
            // The game tick (runtime + embedded renderer) runs on the game thread in the editor; it is
            // executed here for realism but outside the timed editor-layer section.
            if (!skip.contains("tick")) {
                std::lock_guard<std::mutex> lock(window.sceneMutex());
                window.host().gameTick();
            }
            const long long t0 = threadCpuNs();
            window.viewport()->advanceFrame(1.f / 60.f);
            if (!skip.contains("repaint")) {
                window.repaint();
            }
            if (!skip.contains("events")) {
                QCoreApplication::processEvents();
            }
            cpu[static_cast<std::size_t>(f)] = threadCpuNs() - t0;
        }
        heapGrowth = heapInUse() - heapStart;
        rssAfter = residentBytes();
    };

    std::vector<std::vector<long long>> runs;
    std::vector<long long> cpuMin(kFrames, 0);
    long long heapGrowth = 0;
    long long rssEditor = 0;
    auto spikes = [&](const std::vector<long long>& v) {
        std::vector<double> d(v.begin(), v.end());
        const double med = percentile(d, 0.5);
        return std::count_if(v.begin(), v.end(), [&](long long ns) { return ns > kSpikeFactor * med; });
    };
    while (static_cast<int>(runs.size()) < kMinRuns ||
           (static_cast<int>(runs.size()) < kMaxRuns && spikes(cpuMin) != 0)) {
        runs.emplace_back(kFrames);
        long long growth = 0;
        long long rssRun = 0;
        runOnce(runs.back(), growth, rssRun);
        if (runs.size() == 1) {
            rssEditor = rssRun; // later runs re-create the window; RSS is not returned to the OS
        }
        heapGrowth = runs.size() == 1 ? growth : std::min(heapGrowth, growth);
        for (int f = 0; f < kFrames; ++f) {
            cpuMin[static_cast<std::size_t>(f)] = runs.size() == 1 ? runs.back()[static_cast<std::size_t>(f)]
                                                             : std::min(cpuMin[static_cast<std::size_t>(f)],
                                                                        runs.back()[static_cast<std::size_t>(f)]);
        }
        std::vector<double> d(runs.back().begin(), runs.back().end());
        std::printf("run %zu: median %.1f us p99 %.1f us max %.1f us, >3x median: %ld\n", runs.size(),
                    percentile(d, 0.5) / 1e3, percentile(d, 0.99) / 1e3, percentile(d, 1.0) / 1e3,
                    static_cast<long>(spikes(runs.back())));
    }
    std::vector<double> dmin(cpuMin.begin(), cpuMin.end());
    const long spikeCount = static_cast<long>(spikes(cpuMin));
    std::printf("min over %zu runs: median %.1f us p99 %.1f us p99.9 %.1f us max %.1f us, >3x median: %ld\n",
                runs.size(), percentile(dmin, 0.5) / 1e3, percentile(dmin, 0.99) / 1e3, percentile(dmin, 0.999) / 1e3,
                percentile(dmin, 1.0) / 1e3, spikeCount);
    const double mib = 1024.0 * 1024.0;
    const double totalMiB = static_cast<double>(rssEditor - g_rssBeforeQt) / mib;
    const double rendererMiB = static_cast<double>(std::max(rendererRss, 0ll)) / mib;
    const double editorMiB = totalMiB - rendererMiB;
    const double heapMiB = static_cast<double>(heapInUse() - heap0) / mib;
    std::printf("memory: process RSS +%.1f MiB over the pre-QApplication runtime baseline, of which the embedded "
                "runtime renderer (Vulkan/Lavapipe bring-up) +%.1f MiB -> Qt editor layer +%.1f MiB; heap +%.1f MiB "
                "after teardown; heap growth across %d idle frames %+lld bytes\n",
                totalMiB, rendererMiB, editorMiB, heapMiB, kFrames, heapGrowth);
    (void)rss0;
    expect(editorMiB < 256.0, fmt("Qt editor layer memory overhead < 256 MiB (+%.1f MiB RSS)", editorMiB));
    expect(heapGrowth < 1024 * 1024, "no heap creep across 10,000 idle frames (< 1 MiB)");
    if (fuse::core::timingBudgetsEnforced() && releaseBuild()) {
        expect(spikeCount == 0, "no idle Qt editor frame above 3x the median UI-thread CPU time over 10,000 frames");
    } else {
        std::printf("SKIP spike budget (Debug or instrumented build) — stats above are informational\n");
    }
    return g_failures == 0 ? 0 : 1;
}

// ---- present path: Qt adopts the FUSE VkInstance -----------------------------------------------------

int gatePresentAdopt() {
#if defined(FUSE_VULKAN_BACKEND)
    // A VkSurfaceKHR is only valid with the instance that created it (viewportHandoffSurfaceOwnedBy),
    // so the editor must build the viewport surface on the *FUSE* instance. QVulkanInstance can
    // adopt an existing VkInstance (setVkInstance): FUSE creates the instance with the Qt platform's
    // WSI extensions, Qt adopts it and creates the window surface on it, and the hand-off wiring
    // accepts it and builds a real VkSwapchainKHR.
    fuse::renderer::resetVulkanValidationCounters();
    const char* extensions[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = true;
    desc.bootstrap.instance.extraExtensions = extensions;
    desc.bootstrap.instance.extraExtensionCount = 2;
    desc.enableRasterPath = false;
    desc.enableCompositePass = false;
    desc.enableComputePipeline = false;
    std::unique_ptr<fuse::renderer::RhiContext> ctx = fuse::renderer::RhiContext::create(desc);
    expect(ctx != nullptr && ctx->bootstrap().status().deviceReady, "FUSE RhiContext device ready (Lavapipe)");
    if (ctx == nullptr || !ctx->bootstrap().status().deviceReady) {
        return 1;
    }
    const fuse::renderer::VulkanInstanceInfo& info = ctx->bootstrap().instance()->info();
    const bool validation = std::find_if(info.enabledLayers.begin(), info.enabledLayers.end(), [](const char* l) {
                                return std::strcmp(l, "VK_LAYER_KHRONOS_validation") == 0;
                            }) != info.enabledLayers.end();
    std::printf("FUSE instance: validation layer %s, xcb surface ext %s\n", validation ? "on" : "OFF",
                info.instanceHasExtension("VK_KHR_xcb_surface") ? "on" : "OFF");
    expect(info.instanceHasExtension("VK_KHR_xcb_surface"), "FUSE instance carries the Qt xcb WSI extension");
    void* fuseInstance = ctx->bootstrap().instance()->nativeHandle();

    int code = 0;
    {
        QVulkanInstance qtInstance;
        qtInstance.setVkInstance(reinterpret_cast<VkInstance>(fuseInstance));
        expect(qtInstance.create(), "QVulkanInstance adopts the FUSE VkInstance");
        expect(reinterpret_cast<void*>(qtInstance.vkInstance()) == fuseInstance, "adopted handle is FUSE's instance");

        QWindow window;
        window.setSurfaceType(QSurface::VulkanSurface);
        window.setVulkanInstance(&qtInstance);
        window.resize(640, 480);
        window.show();
        expect(QTest::qWaitForWindowExposed(&window, 5000), "Vulkan viewport window exposed");
        const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(&window);
        expect(surface != VK_NULL_HANDLE, "Qt created the window surface on the FUSE instance");

        fuse::editor::ViewportSwapchainHandoff handoff{};
        handoff.nativeSurface = reinterpret_cast<void*>(surface);
        handoff.width = static_cast<fuse::u32>(window.width() * window.devicePixelRatio());
        handoff.height = static_cast<fuse::u32>(window.height() * window.devicePixelRatio());
        handoff.pending = true;
        handoff.qtRealSurface = true;
        handoff.qtVkInstance = reinterpret_cast<void*>(qtInstance.vkInstance());
        handoff.handoffSource = "qt_adopted_fuse_instance";
        expect(fuse::editor::viewportHandoffSurfaceOwnedBy(handoff, fuseInstance),
               "hand-off surface is owned by the RhiContext instance");
        const fuse::editor::ViewportSwapchainWiringResult wired =
            fuse::editor::wireExternalSwapchainFromHandoff(*ctx, handoff);
        const fuse::renderer::VulkanSwapchain* swapchain = ctx->bootstrap().swapchain();
        std::printf("wiring: %s; swapchain %ux%u images %u headless %d\n", wired.note != nullptr ? wired.note : "-",
                    swapchain != nullptr ? swapchain->info().width : 0u, swapchain != nullptr ? swapchain->info().height : 0u,
                    swapchain != nullptr ? swapchain->info().imageCount : 0u,
                    swapchain != nullptr ? static_cast<int>(swapchain->isHeadless()) : -1);
        expect(wired.swapchainReady && !wired.fellBackToHeadless, "real VkSwapchainKHR on the Qt surface (no headless fallback)");
        expect(swapchain != nullptr && !swapchain->isHeadless() && swapchain->hasImages(), "swapchain has presentable images");

        // Resize: rebuild on the same surface.
        window.resize(800, 600);
        QCoreApplication::processEvents();
        fuse::renderer::SwapchainDesc resized{};
        resized.surface.kind = fuse::renderer::SurfaceKind::External;
        resized.surface.nativeSurface = handoff.nativeSurface;
        resized.width = static_cast<fuse::u32>(800 * window.devicePixelRatio());
        resized.height = static_cast<fuse::u32>(600 * window.devicePixelRatio());
        expect(ctx->bootstrap().ensureSwapchain(resized) && !ctx->bootstrap().swapchain()->isHeadless(),
               "swapchain rebuilt for the resized Qt surface");

        // Teardown order: swapchain before the Qt-owned surface, surface before the instance.
        fuse::renderer::SwapchainDesc headless{};
        headless.width = 64;
        headless.height = 64;
        (void)ctx->bootstrap().ensureSwapchain(headless);
        window.destroy();
        qtInstance.destroy(); // adopted: does not destroy FUSE's VkInstance
    }
    ctx.reset();
    const fuse::renderer::VulkanValidationCounters counters = fuse::renderer::vulkanValidationCounters();
    std::printf("validation: %u errors, %u warnings%s%s\n", counters.errors, counters.warnings,
                counters.lastError.empty() ? "" : " — last: ", counters.lastError.c_str());
    if (validation) {
        expect(counters.errors == 0u, "0 Vulkan validation errors across adopt / wire / resize / teardown");
    } else {
        std::printf("note: VK_LAYER_KHRONOS_validation not available — validation-error count not asserted\n");
    }
    std::printf("note: vkQueuePresentKHR itself stays compile-gated (FUSE_TRACK_B_UNLOCK + FUSE_ENABLE_QT_PRESENT)\n");
    return code == 0 && g_failures == 0 ? 0 : 1;
#else
    std::printf("SKIP: Vulkan backend not built\n");
    return 77;
#endif
}


// ---- live present: the running editor presents its viewport ------------------------------------------

#if defined(FUSE_VULKAN_BACKEND)
fuse::editor::WindowPresentStats presentStats(MainWindow& window) {
    return window.host().runtimeViewport().windowPresentStats();
}

/// Mean colour of a centred patch of the viewport as it is on screen (X server contents of the root
/// window over the viewport's global rectangle — what the user sees, not a widget re-render).
QColor screenPatch(QWidget* viewport, const QPoint& local, int half = 6) {
    QScreen* screen = viewport->screen();
    const QPoint g = viewport->mapToGlobal(local) - screen->geometry().topLeft();
    const QImage img = screen->grabWindow(0, g.x() - half, g.y() - half, 2 * half, 2 * half).toImage();
    if (img.isNull()) {
        return {};
    }
    long long r = 0;
    long long gg = 0;
    long long b = 0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            r += c.red();
            gg += c.green();
            b += c.blue();
            ++n;
        }
    }
    return n == 0 ? QColor() : QColor(static_cast<int>(r / n), static_cast<int>(gg / n), static_cast<int>(b / n));
}

std::string colorText(const QColor& c) {
    return fmt("(%d, %d, %d)", c.red(), c.green(), c.blue());
}

bool waitPresented(MainWindow& window, quint64 minPresented, int timeoutMs) {
    return QTest::qWaitFor([&]() { return presentStats(window).presentedImages >= minPresented; }, timeoutMs);
}
#endif

int gateLivePresent() {
#if defined(FUSE_VULKAN_BACKEND)
    if (QGuiApplication::platformName() != QLatin1String("xcb")) {
        std::printf("SKIP: embedded Vulkan viewport needs the xcb platform (have %s)\n",
                    qPrintable(QGuiApplication::platformName()));
        return 77;
    }
    fuse::renderer::resetVulkanValidationCounters();
    using fuse::editor::WindowPresentState;

    {
        MainWindow::Options options; // the real editor: game thread + frame pump running
        options.vulkanValidation = true;
        MainWindow window(samplesRoot(), options);
        expect(showAndExpose(window), "editor main window exposed");
        ViewportPlaceholderWidget* viewport = window.viewport();

        // ---- bring-up: instance published -> adopted -> surface wired -> frames presented ----------
        const bool wired = QTest::qWaitFor(
            [&]() {
                const WindowPresentState state = window.host().runtimeViewport().windowPresentState();
                return state == WindowPresentState::SurfaceWired || state == WindowPresentState::Failed;
            },
            30000);
        const WindowPresentState state = window.host().runtimeViewport().windowPresentState();
        if (state == WindowPresentState::Failed || !wired) {
            std::printf("SKIP: no Vulkan device with xcb WSI (state %d)\n", static_cast<int>(state));
            return 77;
        }
        expect(state == WindowPresentState::SurfaceWired, "game thread wired a real swapchain on the Qt surface");
        expect(viewport->embeddedVulkanViewportActive() && viewport->vulkanContainer() != nullptr &&
                   viewport->vulkanContainer()->isVisible(),
               "Vulkan child window embedded via QWidget::createWindowContainer");
        expect(viewport->vulkanWindow() != nullptr &&
                   viewport->vulkanWindow()->surfaceType() == QSurface::VulkanSurface,
               "viewport window surfaceType == VulkanSurface");
        expect(viewport->vulkanWindow() != nullptr && viewport->vulkanWindow()->vulkanInstance() != nullptr &&
                   reinterpret_cast<void*>(viewport->vulkanWindow()->vulkanInstance()->vkInstance()) ==
                       window.host().runtimeViewport().windowPresentInstance(),
               "Qt adopted the game thread's VkInstance (QVulkanInstance::setVkInstance)");
        expect(fuse::core::trackBHostFeatureEnabled(fuse::core::TrackBHostFeature::EditorViewportPresent) &&
                   !fuse::core::trackBUnlocked(),
               "editor-scoped present unlock on; global FUSE_TRACK_B_UNLOCK untouched");

        expect(waitPresented(window, 10, 20000), "first 10 viewport frames presented");
        const fuse::editor::WindowPresentStats s0 = presentStats(window);
        QTest::qWait(600);
        const fuse::editor::WindowPresentStats s1 = presentStats(window);
        std::printf("present: %llu frames, %llu acquired, %llu presented (+%llu / +%llu in 600 ms), swapchain %ux%u\n",
                    static_cast<unsigned long long>(s1.frames), static_cast<unsigned long long>(s1.acquiredImages),
                    static_cast<unsigned long long>(s1.presentedImages),
                    static_cast<unsigned long long>(s1.acquiredImages - s0.acquiredImages),
                    static_cast<unsigned long long>(s1.presentedImages - s0.presentedImages), s1.width, s1.height);
        expect(s1.acquiredImages > s0.acquiredImages, "swapchain images acquired count increases");
        expect(s1.presentedImages > s0.presentedImages, "vkQueuePresentKHR count increases");
        expect(s1.presentedImages <= s1.acquiredImages, "every presented image was acquired");
        const qreal dpr = viewport->devicePixelRatioF();
        const auto expectedW = static_cast<fuse::u32>(std::lround(viewport->width() * dpr));
        const auto expectedH = static_cast<fuse::u32>(std::lround(viewport->height() * dpr));
        expect(s1.width == expectedW && s1.height == expectedH,
               fmt("swapchain extent %ux%u == viewport %ux%u", s1.width, s1.height, expectedW, expectedH));

        // ---- pixels: the window shows the rendered frame, not the placeholder ------------------------
        const QColor placeholder = viewport->palette().color(QPalette::Window);
        const QColor rendered = screenPatch(viewport, viewport->rect().center());
        const QColor corner = screenPatch(viewport, QPoint(12, 12));
        const QColor softwareFrame = viewport->grab().toImage().pixelColor(12, 12);
        std::printf("pixels: screen centre %s corner %s; placeholder %s (widget re-render %s)\n",
                    colorText(rendered).c_str(), colorText(corner).c_str(), colorText(placeholder).c_str(),
                    colorText(softwareFrame).c_str());
        expect(rendered.isValid() && colorDistance(rendered, placeholder) > 12 &&
                   colorDistance(corner, placeholder) > 12,
               "viewport pixels on screen are the rendered frame, not the software placeholder");
        expect(colorDistance(rendered, corner) > 40,
               "frame carries rendered geometry (raster pass over the clear colour), not a flat fill");
        if (const char* dump = std::getenv("FUSE_QT_PRESENT_DUMP")) {
            QScreen* screen = viewport->screen();
            const QPoint g = viewport->mapToGlobal(QPoint(0, 0)) - screen->geometry().topLeft();
            screen->grabWindow(0, g.x(), g.y(), viewport->width(), viewport->height())
                .save(QString::fromUtf8(dump) + QStringLiteral("_viewport.png"));
            window.grab().save(QString::fromUtf8(dump) + QStringLiteral("_widgets.png"));
        }
        expect(colorDistance(softwareFrame, placeholder) <= 6,
               "control: a software re-render of the widget would show the placeholder colour");

        // ---- resize: swapchain recreated for the new extent, presenting continues -------------------
        const quint64 recreatesBefore = s1.swapchainRecreates;
        window.resize(window.width() - 160, window.height() - 100);
        QCoreApplication::processEvents();
        const bool recreated = QTest::qWaitFor(
            [&]() {
                const fuse::editor::WindowPresentStats s = presentStats(window);
                return s.swapchainRecreates > recreatesBefore &&
                       s.width == static_cast<fuse::u32>(std::lround(viewport->width() * dpr)) &&
                       s.height == static_cast<fuse::u32>(std::lround(viewport->height() * dpr));
            },
            10000);
        const fuse::editor::WindowPresentStats s2 = presentStats(window);
        std::printf("resize: viewport %dx%d -> swapchain %ux%u, recreates %llu -> %llu\n", viewport->width(),
                    viewport->height(), s2.width, s2.height, static_cast<unsigned long long>(recreatesBefore),
                    static_cast<unsigned long long>(s2.swapchainRecreates));
        expect(recreated, "viewport resize recreates the swapchain at the new extent");
        expect(waitPresented(window, s2.presentedImages + 10, 10000), "frames presented after the recreate");
        const QColor afterResize = screenPatch(viewport, QPoint(viewport->width() - 14, viewport->height() - 14));
        std::printf("pixels after resize: bottom-right %s\n", colorText(afterResize).c_str());
        expect(colorDistance(afterResize, corner) <= 6,
               "resized viewport shows the rendered frame edge to edge (new corner == old corner colour)");

        // ---- input forwarded from the Vulkan window to the viewport widget ---------------------------
        fuse::editor::qt::ViewportVulkanWindow* vkWindow = viewport->vulkanWindow();
        const quint64 forwardedBefore = vkWindow->forwardedEventCount();
        const fuse::editor::ViewportCamera cam0 = viewport->panel().camera();
        QTest::mouseClick(vkWindow, Qt::LeftButton, Qt::NoModifier, QPoint(30, 30)); // pick (+ focus)
        // Fly: RMB held + W (WASD only moves while flying).
        QTest::mousePress(vkWindow, Qt::RightButton, Qt::NoModifier, QPoint(40, 40));
        QTest::keyPress(vkWindow, Qt::Key_W);
        QTest::qWait(300);
        QTest::keyRelease(vkWindow, Qt::Key_W);
        QTest::mouseRelease(vkWindow, Qt::RightButton, Qt::NoModifier, QPoint(40, 40));
        const fuse::editor::ViewportCamera cam1 = viewport->panel().camera();
        std::printf("camera z %.3f -> %.3f after RMB+W held 300 ms on the Vulkan window\n", cam0.positionZ,
                    cam1.positionZ);
        expect(cam1.positionZ > cam0.positionZ + 0.5f, "RMB+W on the Vulkan window flies the camera forward");
        expect(viewport->activeContextMenu() == nullptr, "RMB used for flying does not open the context menu");
        QTest::qWait(100);
        const fuse::editor::ViewportCamera cam2 = viewport->panel().camera();
        expect(std::abs(cam2.positionZ - cam1.positionZ) < 1e-4f, "key release stops the camera (no drift)");

        const QPoint centre = viewport->rect().center();
        QTest::mousePress(vkWindow, Qt::RightButton, Qt::NoModifier, centre);
        QTest::mouseMove(vkWindow, centre + QPoint(40, 0));
        QTest::mouseMove(vkWindow, centre + QPoint(80, 0));
        QTest::qWait(60);
        QTest::mouseRelease(vkWindow, Qt::RightButton, Qt::NoModifier, centre + QPoint(80, 0));
        const fuse::editor::ViewportCamera cam3 = viewport->panel().camera();
        std::printf("camera yaw %.2f -> %.2f after RMB drag on the Vulkan window\n", cam2.yaw, cam3.yaw);
        expect(std::abs(cam3.yaw - cam2.yaw) > 1.f, "RMB drag on the Vulkan window turns the camera");
        expect(viewport->activeContextMenu() == nullptr, "RMB drag does not open the context menu");

        QWheelEvent wheel(QPointF(centre), QPointF(vkWindow->mapToGlobal(centre)), QPoint(), QPoint(0, 240),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        const fuse::editor::ViewportCamera cam4 = viewport->panel().camera();
        QCoreApplication::sendEvent(vkWindow, &wheel);
        const fuse::editor::ViewportCamera cam5 = viewport->panel().camera();
        const float moved = std::hypot(cam5.positionX - cam4.positionX, cam5.positionZ - cam4.positionZ);
        std::printf("wheel: camera moved %.3f\n", moved);
        expect(moved > 0.5f, "wheel on the Vulkan window dollies the camera");

        QTest::mouseClick(vkWindow, Qt::RightButton, Qt::NoModifier, QPoint(60, 60));
        const bool menuOpen = QTest::qWaitFor([&]() { return viewport->activeContextMenu() != nullptr; }, 2000);
        expect(menuOpen, "RMB click on the Vulkan window opens the entity context menu");
        if (QMenu* menu = viewport->activeContextMenu()) {
            menu->close();
        }
        expect(vkWindow->forwardedEventCount() > forwardedBefore + 8,
               fmt("Vulkan window forwarded %llu input events to the viewport widget",
                   static_cast<unsigned long long>(vkWindow->forwardedEventCount() - forwardedBefore)));
        QTest::qWait(200);
        expect(presentStats(window).presentedImages > s2.presentedImages, "presenting continued through input");
    }

    // ---- clean teardown: swapchain -> Qt surface -> adopted QVulkanInstance -> FUSE instance ----------
    expect(!fuse::core::trackBHostFeatureEnabled(fuse::core::TrackBHostFeature::EditorViewportPresent),
           "editor present unlock withdrawn at teardown");
    const fuse::renderer::VulkanValidationCounters counters = fuse::renderer::vulkanValidationCounters();
    std::printf("validation: %u errors, %u warnings%s%s\n", counters.errors, counters.warnings,
                counters.lastError.empty() ? "" : " — last: ", counters.lastError.c_str());
    expect(counters.errors == 0u && counters.warnings == 0u,
           "0 VK_LAYER_KHRONOS_validation messages across bring-up / present / resize / input / teardown");
    return g_failures == 0 ? 0 : 1;
#else
    std::printf("SKIP: Vulkan backend not built\n");
    return 77;
#endif
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <startup|layout|theme|font <scale>|wasd|context_menu|fuse_api|ui_frame|"
                             "idle_frames>\n",
                     argv[0]);
        return 2;
    }
    const std::string gate = argv[1];
    const QByteArray platform = qgetenv("QT_QPA_PLATFORM");
    if (platform.isEmpty() && qgetenv("DISPLAY").isEmpty() && qgetenv("WAYLAND_DISPLAY").isEmpty()) {
        std::printf("SKIP: no display (run under xvfb-run or set QT_QPA_PLATFORM=offscreen)\n");
        return 77;
    }
    fuse::core::initialize();
    g_rssBeforeQt = residentBytes();
    const auto t0 = std::chrono::steady_clock::now();
    int code = 0;
    {
        int qtArgc = 1;
        QApplication app(qtArgc, argv);
        fuse::editor::qt::applyEditorTheme(app);
        const double appInitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("platform %s, gate %s\n", qPrintable(QGuiApplication::platformName()), gate.c_str());

        if (gate == "startup") {
            code = gateStartup(appInitMs);
        } else if (gate == "layout") {
            code = gateLayout();
        } else if (gate == "theme") {
            code = gateTheme();
        } else if (gate == "font") {
            code = gateFont(argc > 2 ? std::atof(argv[2]) : 1.0);
        } else if (gate == "wasd") {
            code = gateWasd();
        } else if (gate == "context_menu") {
            code = gateContextMenu();
        } else if (gate == "fuse_api") {
            code = gateFuseApi();
        } else if (gate == "ui_frame") {
            code = gateUiFrame();
        } else if (gate == "present_adopt") {
            code = gatePresentAdopt();
        } else if (gate == "live_present") {
            code = gateLivePresent();
        } else if (gate == "idle_frames") {
            code = gateIdleFrames();
        } else {
            std::fprintf(stderr, "unknown gate %s\n", gate.c_str());
            code = 2;
        }
    }
    fuse::core::shutdown();
    std::printf("%s: %s (%d failure(s))\n", gate.c_str(), code == 0 ? "PASS" : code == 77 ? "SKIP" : "FAIL",
                g_failures);
    return code;
}
