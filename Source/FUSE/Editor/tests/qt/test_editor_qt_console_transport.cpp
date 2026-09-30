// MP-B6-EDITOR-SCRIPT-PIE / UNI-U6-CON-1 Qt gate (offscreen QPA): the Qt console dock and the PIE
// transport drive the real FUSE editor host (fuse_editor_qt).
//
//   console    command line -> ConsolePanel -> CommandQueue -> game tick -> Lua REPL; output drained
//              back into the dock; level colours, level / text filters, "(xN)" repeat counts, Up/Down
//              history navigation, per-level counts on the filter buttons
//   transport  toolbar Play / Pause / Step / Resume / Stop post their commands; the game tick drives
//              PlaySession / PlayModeController; action enable state follows EditorState
#include "editor_panels.hpp"
#include "main_window.hpp"

#include <fuse/core/init.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QTest>
#include <QTextBlock>
#include <QToolBar>
#include <QToolButton>

#include <cstdio>
#include <mutex>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

using fuse::editor::CommandKind;
using fuse::editor::qt::ConsoleWidget;
using fuse::editor::qt::MainWindow;

void tick(MainWindow& window, int frames = 1) {
    for (int i = 0; i < frames; ++i) {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        window.host().gameTick();
    }
    window.console()->drainLog();
    window.updateTransportActions();
    QCoreApplication::processEvents();
}

void submit(MainWindow& window, const char* line) {
    ConsoleWidget* console = window.console();
    console->input()->setText(QString::fromUtf8(line));
    QTest::keyClick(console->input(), Qt::Key_Return);
    tick(window);
}

/// Foreground colour of the first view line that starts with `prefix` (invalid when none).
QColor lineColor(ConsoleWidget* console, const QString& prefix) {
    for (QTextBlock block = console->logView()->document()->begin(); block.isValid(); block = block.next()) {
        if (block.text().startsWith(prefix)) {
            return block.begin().fragment().charFormat().foreground().color();
        }
    }
    return {};
}

int gateConsole(MainWindow& window) {
    ConsoleWidget* console = window.console();
    QPlainTextEdit* view = console->logView();

    submit(window, "print(1+1)");
    expect(view->toPlainText().contains(QStringLiteral("[Info] 2")), "Lua REPL: print(1+1) shows 2 in the dock");
    submit(window, "6 * 7");
    expect(view->toPlainText().contains(QStringLiteral("[Info] 42")), "Lua REPL: bare expression echoes");
    submit(window, "error('qt boom')");
    expect(view->toPlainText().contains(QStringLiteral("qt boom")), "Lua REPL: errors reach the dock");
    submit(window, "stat");
    expect(view->toPlainText().contains(QStringLiteral("ECS entities")), "engine command: stat");

    // Level colours.
    const QColor error = lineColor(console, QStringLiteral("[Error]"));
    const QColor info = lineColor(console, QStringLiteral("[Info]"));
    expect(error.isValid() && error == ConsoleWidget::levelColor(fuse::log::Level::Error), "error lines are red");
    expect(info.isValid() && info == ConsoleWidget::levelColor(fuse::log::Level::Info) && info != error,
           "info lines use the info colour");

    // Per-level counts on the filter buttons.
    QToolButton* errors = console->levelButton(fuse::log::Level::Error);
    QToolButton* infos = console->levelButton(fuse::log::Level::Info);
    expect(errors != nullptr && infos != nullptr && console->levelButton(fuse::log::Level::Trace) != nullptr,
           "level filter buttons present");
    if (errors == nullptr || infos == nullptr) {
        return 1;
    }
    expect(errors->text().startsWith(QStringLiteral("Error (")) && !errors->text().contains(QStringLiteral("(0)")),
           "error button shows the error count");

    // Level filter: hiding Info removes info lines, keeps errors.
    infos->setChecked(false);
    QCoreApplication::processEvents();
    expect(!view->toPlainText().contains(QStringLiteral("[Info] 2")) && view->toPlainText().contains(QStringLiteral("qt boom")),
           "Info filter hides info lines and keeps errors");
    infos->setChecked(true);
    expect(view->toPlainText().contains(QStringLiteral("[Info] 2")), "Info filter restores info lines");

    // Text filter.
    console->textFilter()->setText(QStringLiteral("boom"));
    expect(view->toPlainText().contains(QStringLiteral("qt boom")) && !view->toPlainText().contains(QStringLiteral("[Info] 42")),
           "text filter keeps only matching lines");
    console->textFilter()->clear();
    expect(view->toPlainText().contains(QStringLiteral("[Info] 42")), "clearing the text filter restores the view");

    // Repeat counts.
    console->panel().addLog(fuse::log::Level::Warn, "repeated warning");
    console->panel().addLog(fuse::log::Level::Warn, "repeated warning");
    console->rerender();
    expect(view->toPlainText().contains(QStringLiteral("[Warn] repeated warning (x2)")), "repeat count (x2)");
    console->panel().addLog(fuse::log::Level::Warn, "repeated warning");
    console->rerender();
    expect(view->toPlainText().contains(QStringLiteral("[Warn] repeated warning (x3)")) &&
               !view->toPlainText().contains(QStringLiteral("(x2)")),
           "repeat count updates in place (x3)");
    expect(lineColor(console, QStringLiteral("[Warn]")) == ConsoleWidget::levelColor(fuse::log::Level::Warn),
           "warning lines use the warning colour");

    // History navigation (Up = older, Down = newer).
    QLineEdit* input = console->input();
    input->clear();
    QTest::keyClick(input, Qt::Key_Up);
    expect(input->text() == QStringLiteral("stat"), "Up recalls the last command");
    QTest::keyClick(input, Qt::Key_Up);
    expect(input->text() == QStringLiteral("error('qt boom')"), "Up again recalls the one before");
    QTest::keyClick(input, Qt::Key_Down);
    expect(input->text() == QStringLiteral("stat"), "Down steps back to the newer command");
    QTest::keyClick(input, Qt::Key_Down);
    expect(input->text().isEmpty(), "Down past the newest clears the line");
    return g_failures == 0 ? 0 : 1;
}

int gateTransport(MainWindow& window) {
    fuse::editor::EditorHost& host = window.host();
    QToolBar* bar = window.transportToolBar();
    QAction* play = window.transportAction(CommandKind::StartPlay);
    QAction* pause = window.transportAction(CommandKind::PausePlay);
    QAction* resume = window.transportAction(CommandKind::ResumePlay);
    QAction* step = window.transportAction(CommandKind::StepPlay);
    QAction* stop = window.transportAction(CommandKind::StopPlay);
    expect(bar != nullptr && play && pause && resume && step && stop, "transport toolbar with 5 actions");
    if (bar == nullptr || !play || !pause || !resume || !step || !stop) {
        return 1;
    }
    expect(bar->actions().contains(play) && bar->actions().contains(pause) && bar->actions().contains(resume) &&
               bar->actions().contains(step) && bar->actions().contains(stop),
           "actions live on the toolbar");
    bool inMenu = false;
    for (QAction* menuAction : window.menuBar()->actions()) {
        if (menuAction->menu() != nullptr && menuAction->menu()->actions().contains(step)) {
            inMenu = true;
        }
    }
    expect(inMenu, "Play menu carries the same actions");

    // A moving entity so steps are observable.
    {
        std::lock_guard<std::mutex> lock(window.sceneMutex());
        auto& registry = host.editorScene().registry();
        registry.add(registry.create(), fuse::ecs::Transform{});
    }
    tick(window);
    expect(play->isEnabled() && !pause->isEnabled() && !resume->isEnabled() && !step->isEnabled() && !stop->isEnabled(),
           "stopped: only Play enabled");

    play->trigger();
    tick(window, 3);
    expect(host.editorState().playing && !host.editorState().paused, "Play enters PIE");
    expect(!play->isEnabled() && pause->isEnabled() && stop->isEnabled() && !step->isEnabled(),
           "playing: Pause + Stop enabled");

    pause->trigger();
    tick(window);
    expect(host.editorState().paused, "Pause pauses PIE");
    expect(resume->isEnabled() && step->isEnabled() && !pause->isEnabled(), "paused: Resume + Step enabled");
    const auto steps = host.playSession().sessionTickCount();
    tick(window, 5);
    expect(host.playSession().sessionTickCount() == steps, "paused PIE does not advance");
    step->trigger();
    tick(window);
    expect(host.playSession().manualStepCount() == 1u && host.playSession().sessionTickCount() == steps + 1u,
           "Step advances exactly one step");

    resume->trigger();
    tick(window, 2);
    expect(host.editorState().playing && !host.editorState().paused &&
               host.playSession().sessionTickCount() > steps + 1u,
           "Resume continues PIE");

    stop->trigger();
    tick(window);
    expect(!host.editorState().playing && play->isEnabled() && !stop->isEnabled(), "Stop leaves PIE");
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    fuse::core::initialize();
    int code = 0;
    {
        int qtArgc = 1;
        QApplication app(qtArgc, argv);
        MainWindow::Options options;
        options.startGameLoop = false;
        options.startFramePump = false;
        options.embeddedVulkanViewport = false;
        MainWindow window(QString(), options);
        window.show();
        QCoreApplication::processEvents();
        const std::string gate = argc > 1 ? argv[1] : "all";
        if (gate == "console" || gate == "all") {
            code |= gateConsole(window);
        }
        if (gate == "transport" || gate == "all") {
            code |= gateTransport(window);
        }
    }
    fuse::core::shutdown();
    std::printf("qt console/transport: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
