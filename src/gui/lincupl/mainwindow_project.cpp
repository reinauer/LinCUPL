// Projects: File > New Project..., Open Project..., Close Project.
//
// A project is a .PLD design, as in the original IDE; there is no separate
// project file.  While a project is open, activeDesignPldPath() and
// activeDesignSiPath() return the project's design and its simulation
// input, so Compile, Simulate, the sidebar and the output tabs follow the
// project whichever tab is active.  The last project (project/last), the
// recent projects (recentProjects) and each project's device override
// (project/deviceOverrides, keyed by the design's path) are kept in
// QSettings.

#include "mainwindow.h"

#include "code_editor.h"
#include "tool_paths.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>

namespace {

QString canonicalOrAbsolute(const QString &path) {
    const QFileInfo fi(path);
    const QString canon = fi.canonicalFilePath();
    return canon.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : canon;
}

bool samePath(const QString &a, const QString &b) {
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    return canonicalOrAbsolute(a).compare(canonicalOrAbsolute(b), Qt::CaseInsensitive) == 0;
}

const int kMaxRecentProjects = 8;

} // namespace

QStringList MainWindow::recentProjects() {
    return QSettings().value(QStringLiteral("recentProjects")).toStringList();
}

void MainWindow::addRecentProject(const QString &path) {
    QSettings s;
    QStringList projects = recentProjects();
    projects.removeAll(path);
    projects.prepend(path);
    while (projects.size() > kMaxRecentProjects) {
        projects.removeLast();
    }
    s.setValue(QStringLiteral("recentProjects"), projects);
    rebuildRecentFilesMenu();
}

QString MainWindow::projectName() const {
    return QFileInfo(m_projectPld).completeBaseName();
}

// Opens the design in an editor tab and makes it the project.
bool MainWindow::openProject(const QString &pldPath) {
    if (!QFileInfo(pldPath).isFile()) {
        QMessageBox::warning(this, QStringLiteral("Open Project"),
                             QStringLiteral("The design file does not exist:\n%1").arg(pldPath));
        return false;
    }
    if (!openFile(pldPath, true)) {
        return false;
    }
    m_projectPld = canonicalOrAbsolute(pldPath);

    const QVariantMap overrides = QSettings().value(QStringLiteral("project/deviceOverrides")).toMap();
    m_deviceOverride = overrides.value(m_projectPld).toString();

    QSettings().setValue(QStringLiteral("project/last"), m_projectPld);
    addRecentProject(m_projectPld);
    updateCompanionSidebar();
    updateWindowTitleAndStatus();
    statusBar()->showMessage(QStringLiteral("Project %1 opened.").arg(projectName()), 4000);
    return true;
}

// Closes the tabs of the project's design, its simulation input and its
// output files (asking about unsaved changes) and leaves project mode.
// Returns false, with the project still open, if the user cancels.
bool MainWindow::closeProject() {
    if (m_projectPld.isEmpty()) {
        return true;
    }
    QStringList projectFiles = {m_projectPld};
    for (const QString &ext : {QStringLiteral("si"), QStringLiteral("doc"), QStringLiteral("jed"),
                               QStringLiteral("lst"), QStringLiteral("abs"), QStringLiteral("sim"),
                               QStringLiteral("pla"), QStringLiteral("so"), QStringLiteral("fit"),
                               QStringLiteral("pin"), QStringLiteral("io")}) {
        const QString f = findCompanionFile(m_projectPld, ext);
        if (!f.isEmpty()) {
            projectFiles << f;
        }
    }
    for (int i = m_tabWidget->count() - 1; i >= 0; --i) {
        CodeEditor *ed = editorAt(i);
        if (!ed || ed->filePath().isEmpty()) {
            continue;
        }
        bool belongs = false;
        for (const QString &f : projectFiles) {
            belongs = belongs || samePath(ed->filePath(), f);
        }
        if (!belongs) {
            continue;
        }
        closeTabAt(i);
        if (m_tabWidget->indexOf(ed) >= 0) {
            return false;
        }
    }

    const QString closed = projectName();
    m_projectPld.clear();
    m_deviceOverride.clear();
    QSettings().remove(QStringLiteral("project/last"));
    updateCompanionSidebar();
    updateWindowTitleAndStatus();
    statusBar()->showMessage(QStringLiteral("Project %1 closed.").arg(closed), 4000);
    return true;
}

void MainWindow::restoreLastProject() {
    QSettings s;
    if (!s.value(QStringLiteral("project/reopenLast"), true).toBool()) {
        return;
    }
    const QString last = s.value(QStringLiteral("project/last")).toString();
    if (!last.isEmpty() && QFileInfo(last).isFile()) {
        openProject(last);
    }
}

// The device override chosen in Options > Devices... belongs to the
// project and comes back when it is opened again.
void MainWindow::rememberProjectDeviceOverride() {
    if (m_projectPld.isEmpty()) {
        return;
    }
    QSettings s;
    QVariantMap overrides = s.value(QStringLiteral("project/deviceOverrides")).toMap();
    if (m_deviceOverride.isEmpty()) {
        overrides.remove(m_projectPld);
    } else {
        overrides.insert(m_projectPld, m_deviceOverride);
    }
    s.setValue(QStringLiteral("project/deviceOverrides"), overrides);
}

// With a project open, Compile builds the project's design.  If the active
// tab holds another design, the user chooses between compiling that one
// (which becomes the project) and compiling the project.  Returns false if
// nothing is to be compiled.
bool MainWindow::confirmCompileTarget() {
    if (m_projectPld.isEmpty()) {
        return true;
    }
    CodeEditor *cur = currentEditor();
    if (!cur || cur->isCompanionOutput()) {
        return true;
    }
    const QString name = cur->filePath().isEmpty() ? cur->property("untitledName").toString()
                                                   : QFileInfo(cur->filePath()).fileName();
    if (!name.endsWith(QStringLiteral(".pld"), Qt::CaseInsensitive) ||
        samePath(cur->filePath(), m_projectPld)) {
        return true;
    }

    ProjectSwitch answer = ProjectSwitch::Cancel;
    if (m_projectSwitchPrompt) {
        answer = m_projectSwitchPrompt(name);
    } else {
        QMessageBox box(QMessageBox::Question, QStringLiteral("Compile"),
                        QStringLiteral("%1 is not the design of the current project %2.\n\n"
                                       "Compile %1 and make it the current project?")
                            .arg(name, projectName()),
                        QMessageBox::NoButton, this);
        QPushButton *switchBtn = box.addButton(QStringLiteral("Compile %1").arg(name), QMessageBox::YesRole);
        QPushButton *projectBtn = box.addButton(QStringLiteral("Compile Project %1").arg(projectName()),
                                                QMessageBox::NoRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(switchBtn);
        box.exec();
        if (box.clickedButton() == switchBtn) {
            answer = ProjectSwitch::CompileActiveAsProject;
        } else if (box.clickedButton() == projectBtn) {
            answer = ProjectSwitch::CompileProject;
        }
    }

    switch (answer) {
    case ProjectSwitch::CompileProject:
        return true;
    case ProjectSwitch::CompileActiveAsProject:
        if (cur->filePath().isEmpty() && !saveCurrentTab()) {
            return false;
        }
        return openProject(cur->filePath());
    case ProjectSwitch::Cancel:
        break;
    }
    return false;
}

// The design wizard; its result is saved (asking for a file name if the
// wizard was given none) and opened as the new project.
void MainWindow::actionNewProject() {
    QString defDir = QDir::currentPath();
    if (!m_projectPld.isEmpty()) {
        defDir = QFileInfo(m_projectPld).absolutePath();
    } else if (CodeEditor *ed = currentEditor(); ed && !ed->filePath().isEmpty()) {
        defDir = QFileInfo(ed->filePath()).absolutePath();
    } else {
        const QString exDir = ToolPaths::findExamplesDir();
        if (!exDir.isEmpty()) defDir = exDir;
    }

    NewDesignDialog dlg(&m_deviceLib, defDir, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const NewDesignSpec s = dlg.spec();
    QString path = s.saveFilePath;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save New Project Design"),
            QDir(defDir).filePath(s.name + QStringLiteral(".pld")),
            QStringLiteral("CUPL Designs (*.pld *.PLD);;All Files (*)"));
        if (path.isEmpty()) {
            return;
        }
    }
    createNewTabWithContent(QFileInfo(path).fileName(), NewDesignDialog::generatePldTemplate(s), path);
    if (QFileInfo(path).isFile()) {
        openProject(path);
    }
}

void MainWindow::actionOpenProject() {
    QString startDir = m_projectPld.isEmpty() ? QString() : QFileInfo(m_projectPld).absolutePath();
    if (startDir.isEmpty()) {
        if (CodeEditor *ed = currentEditor(); ed && !ed->filePath().isEmpty()) {
            startDir = QFileInfo(ed->filePath()).absolutePath();
        } else {
            startDir = ToolPaths::findExamplesDir();
        }
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Project"), startDir,
        QStringLiteral("CUPL Designs (*.pld *.PLD);;All Files (*)"));
    if (!path.isEmpty()) {
        openProject(path);
    }
}

void MainWindow::actionCloseProject() {
    closeProject();
}
