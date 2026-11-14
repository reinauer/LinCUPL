#pragma once

#include "compiler_options.h"
#include "device_library.h"
#include "dialogs.h"
#include "error_catalog.h"
#include "font_settings.h"
#include "user_tools.h"

#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

class CodeEditor;
class FindReplaceDialog;
class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QTabWidget;
class QToolBar;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Programmatic API (used by CLI startup & --self-test)
    bool openFile(const QString &filePath, bool activateTab = true);
    void createNewTabWithContent(const QString &titleHint,
                                 const QString &content,
                                 const QString &savePath = QString());
    bool saveTab(int tabIndex, bool saveAs = false);
    bool saveCurrentTab();

    void startCompile(bool deviceIndependent = false);
    void startSimulate(bool deviceIndependent = false);
    void startBackpin(const BackpinRequest &req);
    bool waitForActiveProcess(int timeoutMs = 15000);

    int lastProcessExitCode() const { return m_lastExitCode; }
    bool lastRunSucceeded() const { return m_lastRunOk; }
    QString buildLogText() const;

    const DeviceLibrary &deviceLibrary() const { return m_deviceLib; }
    DeviceLibrary &deviceLibrary() { return m_deviceLib; }
    const ErrorCatalog &errorCatalog() const { return m_errorCatalog; }
    const QList<DiagnosticItem> &currentDiagnostics() const { return m_diagnostics; }

    CodeEditor *currentEditor() const;
    CodeEditor *editorAt(int index) const;
    CodeEditor *editorForPath(const QString &filePath) const;
    int tabCount() const;
    void refreshCompanionTabs(const QString &designSourcePath, bool autoOpenGenerated = true);
    void activateDiagnostic(int index);
    bool updateActivePldDevice(const QString &mnemonic);

    CompilerOptions &compilerOptions() { return m_compilerOpts; }
    SimulatorOptions &simulatorOptions() { return m_simulatorOpts; }

    // Fonts (Options > Font..., mainwindow_fonts.cpp)
    const FontSettings &fontSettings() const { return m_fonts; }
    void applyFontSettings(const FontSettings &fonts);

    // Projects (File > New/Open/Close Project, mainwindow_project.cpp).  A
    // project is a .PLD design: while one is open, Compile and Simulate
    // work on it whichever tab is active, and the sidebar lists its files.
    bool openProject(const QString &pldPath);
    bool closeProject();
    QString projectPath() const { return m_projectPld; }
    void restoreLastProject();
    static QStringList recentProjects();

    // What to do when Compile is started on a .PLD tab that is not the
    // project's design: compile that design and make it the project,
    // compile the project, or nothing.  The default asks the user.
    enum class ProjectSwitch { CompileActiveAsProject, CompileProject, Cancel };
    using ProjectSwitchPrompt = std::function<ProjectSwitch(const QString &activePld)>;
    void setProjectSwitchPrompt(ProjectSwitchPrompt prompt) { m_projectSwitchPrompt = std::move(prompt); }

    // User tools (Tools menu, mainwindow_tools.cpp)
    const QList<UserTool> &userTools() const { return m_userTools; }
    void setUserTools(const QList<UserTool> &tools);
    void runUserTool(int index);
    void stopRunningTool();
    ToolContext userToolContext() const;

    // Utilities > Float All Pins and Nodes, Assign Pins and Pinnodes,
    // Assign Pins Only: backpin -f, backpin, backpin -p on the project's
    // design (mainwindow_pins.cpp).
    enum class PinPlacement { FloatAll, AssignPinsAndNodes, AssignPinsOnly };
    struct PinPlacementPlan {
        QString error;          // why it cannot run; empty if it can
        BackpinRequest request;
        QString placementFile;  // fitter output the pins come from (assign only)
        QString generatedPlc;   // .plc to write from the fitter's .pin report
        bool stale = false;     // placementFile is older than the design
    };
    PinPlacementPlan planPinPlacement(PinPlacement mode) const;
    void startPinPlacement(PinPlacement mode);
    // True if `after` differs from `before` only in the pin and node numbers
    // of PIN, PINNODE and NODE statements and in white space: all that
    // backpin may change.  backpin stops at statements it cannot rewrite and
    // leaves the rest of the design out of the file it writes.
    static bool backpinKeptDesign(const QString &before, const QString &after);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // File menu
    void actionNewProject();
    void actionOpenProject();
    void actionCloseProject();
    void actionNewEmptyPld();
    void actionNewSimulationWizard();
    void actionOpenFile();
    void actionReloadFromDisk();
    void actionSave();
    void actionSaveAs();
    void actionSaveAll();
    void actionPrint();
    void actionCloseCurrentTab();
    void closeTabAt(int index);

    // Edit menu
    void actionFind();
    void actionReplace();
    void actionGotoLine();
    void actionInsertConstruct();
    void actionTableEditor();
    void runMacroDialog(int mode);

    // Options menu
    void actionSelectDevice();
    void actionCompilerOptions();
    void actionSimulatorOptions();
    void actionEnvironmentPaths();
    void actionFont();

    // Run & Utilities menus
    void actionCompileDependent();
    void actionCompileIndependent();
    void actionSimulateDependent();
    void actionSimulateIndependent();
    void actionCompileAndSimulate();
    void actionLaunchLinSim();
    void actionRunBackpin();
    void actionInspectLibraryToc();

    // Tools menu
    void actionAddTool();
    void actionEditTools();

    // Help menu
    void actionErrorMessagesCatalog();
    void actionAbout();

    // Internal UI updates
    void onCurrentTabChanged(int index);
    void onDiagnosticItemClicked(QTreeWidgetItem *item, int column);
    void onDiagnosticItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onCompanionItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void rebuildWindowMenu();

private:
    enum class JobKind {
        None,
        Compile,
        CompileThenSimulate,
        Simulate,
        Backpin,
        CbldToc,
        UserTool
    };

    void setupDataFiles();
    void buildUi();
    void buildMenusAndToolbars();
    void updateWindowTitleAndStatus();
    void updateTabTitle(int index);
    bool maybeSaveAllBeforeClose();
    bool ensureActiveSourceSaved(const QStringList &allowedExts, QString *resolvedPathOut);
    QString activeDesignPldPath() const;
    QString activeDesignSiPath() const;
    QString extractPldDeviceMnemonic(const QString &text) const;
    QString extractPldHeaderName(const QString &text) const;
    QString findCompanionFile(const QString &baseSourcePath, const QString &extWithoutDot) const;
    void updateCompanionSidebar();
    void addRecentFile(const QString &path);
    void rebuildRecentFilesMenu();
    void addRecentProject(const QString &path);
    bool confirmCompileTarget();
    void rememberProjectDeviceOverride();
    QString projectName() const;
    void buildToolsMenu();
    void rebuildUserToolActions();
    void finishUserTool(int exitCode, QProcess::ExitStatus exitStatus);
    void finishBackpin();
    void reloadEditorKeepingUndo(const QString &path);
    void applyDiagnosticsToEditors();
    void showErrorExplanation(const DiagnosticItem &diag);
    void runToolCommand(JobKind kind,
                        const QString &program,
                        const QStringList &args,
                        const QString &workingDir,
                        const QString &primarySourcePath);

    static bool isCompanionOutputExtension(const QString &filePath);

    DeviceLibrary m_deviceLib;
    ErrorCatalog m_errorCatalog;
    CompilerOptions m_compilerOpts;
    SimulatorOptions m_simulatorOpts;
    FontSettings m_fonts;
    QString m_deviceOverride;
    QString m_projectPld;
    ProjectSwitchPrompt m_projectSwitchPrompt;

    QTabWidget *m_tabWidget = nullptr;
    QDockWidget *m_companionDock = nullptr;
    QTreeWidget *m_companionTree = nullptr;
    QDockWidget *m_bottomDock = nullptr;
    QTabWidget *m_bottomTabs = nullptr;
    QTreeWidget *m_diagTree = nullptr;
    QTextBrowser *m_errorExplainBrowser = nullptr;
    QPlainTextEdit *m_buildLogEdit = nullptr;

    QMenu *m_recentMenu = nullptr;
    QMenu *m_windowMenu = nullptr;
    QLabel *m_statusProjectLabel = nullptr;
    QAction *m_closeProjectAction = nullptr;
    QMenu *m_toolsMenu = nullptr;
    QToolBar *m_userToolBar = nullptr;
    QAction *m_stopToolAction = nullptr;
    QList<UserTool> m_userTools;
    bool m_stopRequested = false;
    QString m_generatedPlcPath;
    QByteArray m_backpinSourceBefore;   // the design as it was when backpin started
    bool m_backpinHaveSource = false;
    QLabel *m_statusDeviceLabel = nullptr;
    QLabel *m_statusCursorLabel = nullptr;
    QLabel *m_statusLibLabel = nullptr;
    FindReplaceDialog *m_findDialog = nullptr;

    QProcess *m_process = nullptr;
    JobKind m_activeJob = JobKind::None;
    bool m_activeJobDeviceIndependent = false;
    QString m_activeJobSourcePath;
    QString m_accumulatedOutput;
    QElapsedTimer m_jobTimer;
    int m_lastExitCode = 0;
    bool m_lastRunOk = true;
    QList<DiagnosticItem> m_diagnostics;
};
