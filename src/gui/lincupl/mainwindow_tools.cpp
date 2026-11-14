// The Tools menu: up to 13 external programs defined by the user (Tools >
// Add Tool..., Edit Tools...), each started with placeholders such as
// $(JED) and $(DEVICE) filled in from the project, either with its output
// in the build log (stoppable) or detached.  Tools marked for the toolbar
// also get a button on the user tools toolbar.

#include "mainwindow.h"

#include "../common/action_icons.h"
#include "code_editor.h"
#include "tool_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>

void MainWindow::buildToolsMenu() {
    m_toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
    m_userToolBar = addToolBar(QStringLiteral("User Tools"));
    m_userToolBar->setObjectName(QStringLiteral("UserToolsToolBar"));
    m_userToolBar->setIconSize(QSize(24, 24));
    m_userToolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    m_stopToolAction = new QAction(lincuplIcon(QStringLiteral("stop")), QStringLiteral("&Stop Running Tool"), this);
    m_stopToolAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F9")));
    m_stopToolAction->setToolTip(QStringLiteral("Stop Running Tool (%1)")
                                     .arg(m_stopToolAction->shortcut().toString(QKeySequence::NativeText)));
    m_stopToolAction->setEnabled(false);
    connect(m_stopToolAction, &QAction::triggered, this, &MainWindow::stopRunningTool);

    m_userTools = UserTool::loadFromSettings();
    rebuildUserToolActions();
}

// The tools in the menu, then Stop, Add Tool... and Edit Tools...; the
// toolbar holds the tools marked for it and is hidden when there are none.
void MainWindow::rebuildUserToolActions() {
    m_toolsMenu->clear();
    m_userToolBar->clear();
    for (int i = 0; i < m_userTools.size(); ++i) {
        const UserTool &tool = m_userTools.at(i);
        auto *act = new QAction(UserTools::iconFor(tool), tool.menuName, m_toolsMenu);
        act->setToolTip(tool.toolTip.isEmpty() ? tool.menuName : tool.toolTip);
        act->setStatusTip(QStringLiteral("%1 %2").arg(tool.command, tool.arguments));
        connect(act, &QAction::triggered, this, [this, i]() { runUserTool(i); });
        m_toolsMenu->addAction(act);
        if (tool.onToolbar) {
            m_userToolBar->addAction(act);
        }
    }
    if (m_userTools.isEmpty()) {
        m_toolsMenu->addAction(QStringLiteral("(No Tools Defined)"))->setEnabled(false);
    }
    m_toolsMenu->addSeparator();
    m_toolsMenu->addAction(m_stopToolAction);
    m_toolsMenu->addSeparator();
    QAction *add = m_toolsMenu->addAction(lincuplIcon(QStringLiteral("tool")), QStringLiteral("&Add Tool..."),
                                          this, &MainWindow::actionAddTool);
    add->setEnabled(m_userTools.size() < UserTool::kMaxTools);
    m_toolsMenu->addAction(QStringLiteral("&Edit Tools..."), this, &MainWindow::actionEditTools);

    const bool anyOnToolbar = !m_userToolBar->actions().isEmpty();
    if (anyOnToolbar) {
        m_userToolBar->addSeparator();
        m_userToolBar->addAction(m_stopToolAction);
    }
    m_userToolBar->setVisible(anyOnToolbar);
}

void MainWindow::setUserTools(const QList<UserTool> &tools) {
    m_userTools = tools.mid(0, UserTool::kMaxTools);
    UserTool::saveToSettings(m_userTools);
    rebuildUserToolActions();
}

// The placeholder values: the project's (or active) design and its files.
ToolContext MainWindow::userToolContext() const {
    ToolContext ctx;
    if (CodeEditor *cur = currentEditor()) {
        ctx.file = cur->filePath();
    }
    ctx.design = activeDesignPldPath();
    const QString base = ctx.design.isEmpty() ? ctx.file : ctx.design;
    if (!base.isEmpty()) {
        const QFileInfo fi(base);
        ctx.dir = fi.absolutePath();
        ctx.name = fi.completeBaseName();
    }
    if (!ctx.design.isEmpty()) {
        ctx.jed = findCompanionFile(ctx.design, QStringLiteral("jed"));
        if (ctx.jed.isEmpty()) {
            ctx.jed = QDir(ctx.dir).filePath(ctx.name + QStringLiteral(".jed"));
        }
        ctx.si = findCompanionFile(ctx.design, QStringLiteral("si"));
    }
    ctx.device = m_deviceOverride;
    if (ctx.device.isEmpty() && !ctx.design.isEmpty()) {
        if (CodeEditor *ed = editorForPath(ctx.design)) {
            ctx.device = extractPldDeviceMnemonic(ed->toPlainText());
        } else {
            QFile f(ctx.design);
            if (f.open(QIODevice::ReadOnly)) {
                ctx.device = extractPldDeviceMnemonic(QString::fromLatin1(f.readAll()));
            }
        }
    }
    ctx.library = m_deviceLib.libraryPath().isEmpty() ? ToolPaths::activeDeviceLibrary()
                                                      : m_deviceLib.libraryPath();
    return ctx;
}

void MainWindow::runUserTool(int index) {
    if (index < 0 || index >= m_userTools.size()) {
        return;
    }
    const UserTool tool = m_userTools.at(index);
    if (tool.saveFirst) {
        actionSaveAll();
    }
    const ToolContext ctx = userToolContext();

    QString program = UserTools::resolveProgram(UserTools::expandPlaceholders(tool.command, ctx));
    QStringList args = UserTools::expandArguments(tool.arguments, ctx);
    QString workDir = tool.workingDir.isEmpty() ? ctx.dir : UserTools::expandPlaceholders(tool.workingDir, ctx);
    if (workDir.isEmpty() || !QFileInfo(workDir).isDir()) {
        workDir = QDir::currentPath();
    }
#ifdef Q_OS_MACOS
    // An application bundle is started through open(1); -W waits for it
    // when its run is followed in the build log.
    if (QFileInfo(program).isDir() && program.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive)) {
        QStringList openArgs;
        if (tool.captureOutput) openArgs << QStringLiteral("-W");
        openArgs << QStringLiteral("-a") << program << QStringLiteral("--args") << args;
        program = QStringLiteral("/usr/bin/open");
        args = openArgs;
    }
#endif

    if (tool.captureOutput) {
        m_stopRequested = false;
        runToolCommand(JobKind::UserTool, program, args, workDir, ctx.design);
        if (m_activeJob == JobKind::UserTool) {
            statusBar()->showMessage(QStringLiteral("Running %1...").arg(tool.menuName));
        }
        return;
    }
    if (QProcess::startDetached(program, args, workDir)) {
        statusBar()->showMessage(QStringLiteral("Started %1").arg(tool.menuName), 4000);
    } else {
        QMessageBox::warning(this, tool.menuName,
                             QStringLiteral("Could not start:\n%1").arg(program));
    }
}

// Stops whatever runs with its output in the build log: a user tool or a
// compile, simulation or backpin run.
void MainWindow::stopRunningTool() {
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_stopRequested = true;
    m_buildLogEdit->appendPlainText(QStringLiteral("\n=== Stopped by the user ==="));
    m_process->kill();
}

void MainWindow::finishUserTool(int exitCode, QProcess::ExitStatus exitStatus) {
    m_diagnostics.clear();
    m_lastRunOk = !m_stopRequested && exitStatus == QProcess::NormalExit && exitCode == 0;
    statusBar()->showMessage(m_stopRequested ? QStringLiteral("Tool stopped.")
                             : m_lastRunOk   ? QStringLiteral("Tool finished.")
                                             : QStringLiteral("Tool finished with exit code %1.").arg(m_lastExitCode),
                             5000);
    m_stopRequested = false;
    updateCompanionSidebar();
}

void MainWindow::actionAddTool() {
    if (m_userTools.size() >= UserTool::kMaxTools) {
        QMessageBox::information(this, QStringLiteral("Add Tool"),
                                 QStringLiteral("There can be at most %1 tools.").arg(UserTool::kMaxTools));
        return;
    }
    UserToolDialog dlg(userToolContext(), this);
    if (dlg.exec() == QDialog::Accepted) {
        QList<UserTool> tools = m_userTools;
        tools << dlg.tool();
        setUserTools(tools);
    }
}

void MainWindow::actionEditTools() {
    UserToolsDialog dlg(m_userTools, userToolContext(), this);
    if (dlg.exec() == QDialog::Accepted) {
        setUserTools(dlg.tools());
    }
}
