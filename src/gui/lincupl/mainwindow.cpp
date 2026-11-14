#include "mainwindow.h"

#include "../common/action_icons.h"
#include "../common/help_window.h"
#include "code_editor.h"
#include "dialogs.h"
#include "tool_paths.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#ifdef LINCUPL_HAS_PRINTSUPPORT
#include <QPrintDialog>
#include <QPrinter>
#endif

#ifdef LINCUPL_HAS_ABSDUMP
#include <cstdlib>
#include "absdump.h"
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("LinCUPL - Universal Compiler for Programmable Logic IDE"));
    resize(1240, 820);

    m_compilerOpts.loadFromSettings();
    m_simulatorOpts.loadFromSettings();
    setupDataFiles();
    buildUi();
    buildMenusAndToolbars();
    m_fonts.loadFromSettings();
    applyFontSettings(m_fonts);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &MainWindow::onProcessReadyRead);
    connect(m_process, &QProcess::finished, this, &MainWindow::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &MainWindow::onProcessError);
    connect(m_process, &QProcess::stateChanged, this, [this](QProcess::ProcessState state) {
        m_stopToolAction->setEnabled(state != QProcess::NotRunning);
    });

    m_findDialog = new FindReplaceDialog([this]() { return currentEditor(); }, this);

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("ui/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("ui/windowState")).toByteArray());

    updateWindowTitleAndStatus();
}

MainWindow::~MainWindow() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

void MainWindow::setupDataFiles() {
    const QString dlPath = ToolPaths::activeDeviceLibrary();
    if (!dlPath.isEmpty()) {
        m_deviceLib.loadLibrary(dlPath);
    }
    const QString catPath = ToolPaths::findAtmelCat();
    if (!catPath.isEmpty()) {
        m_deviceLib.loadCategories(catPath);
    }
    const QString errPath = ToolPaths::findCuplErrTxt();
    if (!errPath.isEmpty()) {
        m_errorCatalog.loadCatalog(errPath);
    }
}

void MainWindow::buildUi() {
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabsClosable(true);
    m_tabWidget->setMovable(true);
    m_tabWidget->setDocumentMode(true);
    connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTabAt);
    connect(m_tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    setCentralWidget(m_tabWidget);

    // Left Dock: Project & Companion Output Files
    m_companionDock = new QDockWidget(QStringLiteral("Design && Output Files"), this);
    m_companionDock->setObjectName(QStringLiteral("CompanionFilesDock"));
    m_companionTree = new QTreeWidget(m_companionDock);
    m_companionTree->setRootIsDecorated(false);
    m_companionTree->setAlternatingRowColors(true);
    m_companionTree->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Role"), QStringLiteral("Status")});
    m_companionTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    connect(m_companionTree, &QTreeWidget::itemDoubleClicked,
            this, &MainWindow::onCompanionItemDoubleClicked);
    m_companionDock->setWidget(m_companionTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_companionDock);

    // Bottom Dock: Diagnostics + CuplErr.txt Explanation + Live Build Log
    m_bottomDock = new QDockWidget(QStringLiteral("Diagnostics && Build Output"), this);
    m_bottomDock->setObjectName(QStringLiteral("BuildDiagnosticsDock"));
    m_bottomTabs = new QTabWidget(m_bottomDock);

    auto *diagContainer = new QWidget(m_bottomTabs);
    auto *diagLayout = new QVBoxLayout(diagContainer);
    diagLayout->setContentsMargins(4, 4, 4, 4);
    auto *diagSplitter = new QSplitter(Qt::Horizontal, diagContainer);

    m_diagTree = new QTreeWidget(diagSplitter);
    m_diagTree->setRootIsDecorated(false);
    m_diagTree->setAlternatingRowColors(true);
    m_diagTree->setHeaderLabels({
        QStringLiteral("Severity"),
        QStringLiteral("Code"),
        QStringLiteral("File"),
        QStringLiteral("Line"),
        QStringLiteral("Message")
    });
    m_diagTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_diagTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_diagTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_diagTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    connect(m_diagTree, &QTreeWidget::itemClicked, this, &MainWindow::onDiagnosticItemClicked);
    connect(m_diagTree, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onDiagnosticItemDoubleClicked);

    auto *explainPane = new QWidget(diagSplitter);
    auto *explainVBox = new QVBoxLayout(explainPane);
    explainVBox->setContentsMargins(0, 0, 0, 0);
    auto *explainHdr = new QHBoxLayout();
    explainHdr->addWidget(new QLabel(QStringLiteral("<b>CuplErr.txt Explanation:</b>"), explainPane));
    explainHdr->addStretch();
    auto *openCatalogBtn = new QPushButton(QStringLiteral("Search Error Catalog..."), explainPane);
    connect(openCatalogBtn, &QPushButton::clicked, this, &MainWindow::actionErrorMessagesCatalog);
    explainHdr->addWidget(openCatalogBtn);
    explainVBox->addLayout(explainHdr);

    m_errorExplainBrowser = new QTextBrowser(explainPane);
    m_errorExplainBrowser->setPlaceholderText(
        QStringLiteral("Click any compiler or simulator diagnostic on the left to jump to its source line and view its CuplErr.txt explanation here."));
    explainVBox->addWidget(m_errorExplainBrowser, 1);

    diagSplitter->setStretchFactor(0, 3);
    diagSplitter->setStretchFactor(1, 2);
    diagLayout->addWidget(diagSplitter);
    m_bottomTabs->addTab(diagContainer, QStringLiteral("Diagnostics (0)"));

    m_buildLogEdit = new QPlainTextEdit(m_bottomTabs);
    m_buildLogEdit->setReadOnly(true);
    m_buildLogEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_bottomTabs->addTab(m_buildLogEdit, QStringLiteral("Build && Simulation Log"));

    m_bottomDock->setWidget(m_bottomTabs);
    addDockWidget(Qt::BottomDockWidgetArea, m_bottomDock);

    // Status bar widgets
    m_statusProjectLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusProjectLabel);
    m_statusDeviceLabel = new QLabel(this);
    m_statusLibLabel = new QLabel(this);
    m_statusCursorLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusDeviceLabel);
    statusBar()->addPermanentWidget(m_statusLibLabel);
    statusBar()->addPermanentWidget(m_statusCursorLabel);
}

void MainWindow::buildMenusAndToolbars() {
    auto *tb = addToolBar(QStringLiteral("Main Toolbar"));
    tb->setObjectName(QStringLiteral("MainToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(24, 24));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);

    // --- File Menu ---
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *actNewWiz = fileMenu->addAction(
        lincuplIcon(QStringLiteral("new")),
        QStringLiteral("&New Project..."), QKeySequence::New,
        this, &MainWindow::actionNewProject);
    fileMenu->addAction(QStringLiteral("New &Empty PLD"), this, &MainWindow::actionNewEmptyPld);
    QAction *actNewSi = fileMenu->addAction(
        lincuplIcon(QStringLiteral("new-si")),
        QStringLiteral("New &Simulation (.SI)..."), QKeySequence(QStringLiteral("Ctrl+Shift+N")),
        this, &MainWindow::actionNewSimulationWizard);
    fileMenu->addSeparator();

    QAction *actOpen = fileMenu->addAction(
        lincuplIcon(QStringLiteral("open")),
        QStringLiteral("&Open..."), QKeySequence::Open,
        this, &MainWindow::actionOpenFile);
    fileMenu->addAction(lincuplIcon(QStringLiteral("project")), QStringLiteral("Open Pro&ject..."),
                        QKeySequence(QStringLiteral("Ctrl+Shift+O")),
                        this, &MainWindow::actionOpenProject);
    m_recentMenu = fileMenu->addMenu(QStringLiteral("Open &Recent"));
    rebuildRecentFilesMenu();
    fileMenu->addAction(QStringLiteral("&Reload from Disk"), QKeySequence::Refresh,
                        this, &MainWindow::actionReloadFromDisk);
    fileMenu->addSeparator();

    QAction *actSave = fileMenu->addAction(
        lincuplIcon(QStringLiteral("save")),
        QStringLiteral("&Save"), QKeySequence::Save,
        this, &MainWindow::actionSave);
    fileMenu->addAction(QStringLiteral("Save &As..."), QKeySequence::SaveAs,
                        this, &MainWindow::actionSaveAs);
    fileMenu->addAction(QStringLiteral("Save A&ll"), QKeySequence(QStringLiteral("Ctrl+Alt+S")),
                        this, &MainWindow::actionSaveAll);
    fileMenu->addSeparator();

    QAction *actPrint = fileMenu->addAction(
        lincuplIcon(QStringLiteral("print")), QStringLiteral("&Print..."), QKeySequence::Print,
        this, &MainWindow::actionPrint);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Close Tab"), QKeySequence::Close,
                        this, &MainWindow::actionCloseCurrentTab);
    m_closeProjectAction = fileMenu->addAction(QStringLiteral("Close Projec&t"),
                                               this, &MainWindow::actionCloseProject);
    m_closeProjectAction->setEnabled(false);
    fileMenu->addAction(QStringLiteral("E&xit"), QKeySequence::Quit,
                        this, &QWidget::close);

    // --- Edit Menu ---
    QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    editMenu->addAction(QStringLiteral("&Undo"), QKeySequence::Undo, this, [this]() {
        if (auto *e = currentEditor()) e->undo();
    });
    editMenu->addAction(QStringLiteral("&Redo"), QKeySequence::Redo, this, [this]() {
        if (auto *e = currentEditor()) e->redo();
    });
    editMenu->addSeparator();
    QAction *actCut = editMenu->addAction(lincuplIcon(QStringLiteral("cut")),
                                          QStringLiteral("Cu&t"), QKeySequence::Cut, this, [this]() {
        if (auto *e = currentEditor()) e->cut();
    });
    QAction *actCopy = editMenu->addAction(lincuplIcon(QStringLiteral("copy")),
                                           QStringLiteral("&Copy"), QKeySequence::Copy, this, [this]() {
        if (auto *e = currentEditor()) e->copy();
    });
    QAction *actPaste = editMenu->addAction(lincuplIcon(QStringLiteral("paste")),
                                            QStringLiteral("&Paste"), QKeySequence::Paste, this, [this]() {
        if (auto *e = currentEditor()) e->paste();
    });
    editMenu->addAction(QStringLiteral("Select &All"), QKeySequence::SelectAll, this, [this]() {
        if (auto *e = currentEditor()) e->selectAll();
    });
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("&Find..."), QKeySequence::Find,
                        this, &MainWindow::actionFind);
    editMenu->addAction(QStringLiteral("&Replace..."), QKeySequence(QStringLiteral("Ctrl+H")),
                        this, &MainWindow::actionReplace);
    editMenu->addAction(QStringLiteral("&Go to Line..."), QKeySequence(QStringLiteral("Ctrl+G")),
                        this, &MainWindow::actionGotoLine);
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("Insert CUPL &Construct..."),
                        this, &MainWindow::actionInsertConstruct);
    editMenu->addAction(QStringLiteral("&Binary Table Editor..."),
                        this, &MainWindow::actionTableEditor);
    QMenu *macroMenu = editMenu->addMenu(QStringLiteral("CUPL &Macros"));
    macroMenu->addAction(QStringLiteral("&Define Macro..."), this, [this]() {
        runMacroDialog(static_cast<int>(MacroDialog::Mode::Define));
    });
    macroMenu->addAction(QStringLiteral("&Create Macro in Library..."), this, [this]() {
        runMacroDialog(static_cast<int>(MacroDialog::Mode::Create));
    });
    macroMenu->addAction(QStringLiteral("&Reference Macro..."), this, [this]() {
        runMacroDialog(static_cast<int>(MacroDialog::Mode::Reference));
    });

    // --- View Menu ---
    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(m_companionDock->toggleViewAction());
    viewMenu->addAction(m_bottomDock->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("Refresh &Companion Output Tabs"),
                        QKeySequence(QStringLiteral("Shift+F5")), this, [this]() {
                            const QString pld = activeDesignPldPath();
                            if (!pld.isEmpty()) refreshCompanionTabs(pld, true);
                        });
    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("Zoom &In"), QKeySequence::ZoomIn, this, [this]() {
        if (auto *e = currentEditor()) e->zoomBy(1);
    });
    viewMenu->addAction(QStringLiteral("Zoom &Out"), QKeySequence::ZoomOut, this, [this]() {
        if (auto *e = currentEditor()) e->zoomBy(-1);
    });

    // --- Options Menu ---
    QMenu *optionsMenu = menuBar()->addMenu(QStringLiteral("&Options"));
    QAction *actDevices = optionsMenu->addAction(
        lincuplIcon(QStringLiteral("devices")),
        QStringLiteral("&Devices..."), this, &MainWindow::actionSelectDevice);
    QAction *actCompOpts = optionsMenu->addAction(
        lincuplIcon(QStringLiteral("options")),
        QStringLiteral("&Compiler Options..."), this, &MainWindow::actionCompilerOptions);
    optionsMenu->addAction(QStringLiteral("&Simulator Options..."),
                           this, &MainWindow::actionSimulatorOptions);
    optionsMenu->addSeparator();
    optionsMenu->addAction(lincuplIcon(QStringLiteral("font")), QStringLiteral("&Font..."),
                           this, &MainWindow::actionFont);
    optionsMenu->addAction(QStringLiteral("&Environment && Tool Paths..."),
                           this, &MainWindow::actionEnvironmentPaths);
    QAction *actReopen = optionsMenu->addAction(QStringLiteral("&Reopen Last Project at Startup"));
    actReopen->setCheckable(true);
    actReopen->setChecked(QSettings().value(QStringLiteral("project/reopenLast"), true).toBool());
    connect(actReopen, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("project/reopenLast"), on);
    });

    // --- Run Menu ---
    QMenu *runMenu = menuBar()->addMenu(QStringLiteral("&Run"));
    QAction *actCompDep = runMenu->addAction(
        lincuplIcon(QStringLiteral("compile")),
        QStringLiteral("&Device Dependent Compile"), QKeySequence(Qt::Key_F9),
        this, &MainWindow::actionCompileDependent);
    QAction *actCompIndep = runMenu->addAction(
        lincuplIcon(QStringLiteral("compile-virtual")),
        QStringLiteral("Device &Independent Compile (virtual)"),
        QKeySequence(QStringLiteral("Shift+F9")),
        this, &MainWindow::actionCompileIndependent);
    runMenu->addSeparator();
    QAction *actSimDep = runMenu->addAction(
        lincuplIcon(QStringLiteral("simulate")),
        QStringLiteral("Device Dependent &Simulate"), QKeySequence(Qt::Key_F10),
        this, &MainWindow::actionSimulateDependent);
    QAction *actSimIndep = runMenu->addAction(
        lincuplIcon(QStringLiteral("simulate-virtual")),
        QStringLiteral("Device Independent Sim&ulate (virtual)"),
        QKeySequence(QStringLiteral("Shift+F10")),
        this, &MainWindow::actionSimulateIndependent);
    runMenu->addAction(
        lincuplIcon(QStringLiteral("compile-simulate")),
        QStringLiteral("Compile && Simulate"), QKeySequence(Qt::Key_F11),
        this, &MainWindow::actionCompileAndSimulate);
    runMenu->addSeparator();
    QAction *actLinSim = runMenu->addAction(
        lincuplIcon(QStringLiteral("app-linsim")),
        QStringLiteral("Launch &Waveform Simulator (linsim)"), QKeySequence(Qt::Key_F12),
        this, &MainWindow::actionLaunchLinSim);

    // --- Utilities Menu ---
    QMenu *utilMenu = menuBar()->addMenu(QStringLiteral("&Utilities"));
    utilMenu->addAction(QStringLiteral("&Backpin..."), this, &MainWindow::actionRunBackpin);
    utilMenu->addAction(lincuplIcon(QStringLiteral("float-pins")), QStringLiteral("&Float All Pins and Nodes"),
                        this, [this]() { startPinPlacement(PinPlacement::FloatAll); });
    utilMenu->addAction(lincuplIcon(QStringLiteral("assign-pins")), QStringLiteral("&Assign Pins and Pinnodes"),
                        this, [this]() { startPinPlacement(PinPlacement::AssignPinsAndNodes); });
    utilMenu->addAction(QStringLiteral("Assign &Pins Only"),
                        this, [this]() { startPinPlacement(PinPlacement::AssignPinsOnly); });
    utilMenu->addSeparator();
    utilMenu->addAction(QStringLiteral("Device Library Table of Contents (&cbld -t)..."),
                        this, &MainWindow::actionInspectLibraryToc);

    // --- Tools Menu (and the user tools toolbar) ---
    buildToolsMenu();

    // --- Window Menu ---
    m_windowMenu = menuBar()->addMenu(QStringLiteral("&Window"));
    connect(m_windowMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildWindowMenu);

    // --- Help Menu ---
    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    Help::addHelpMenuActions(helpMenu, Help::Context::Design);
    Help::setActionHandler(QStringLiteral("error-messages"),
                           [this]() { actionErrorMessagesCatalog(); });
    helpMenu->addSeparator();
    QAction *actErrHelp = helpMenu->addAction(
        lincuplIcon(QStringLiteral("error-help")),
        QStringLiteral("&Error Messages..."),
        this, &MainWindow::actionErrorMessagesCatalog);
    helpMenu->addSeparator();
    helpMenu->addAction(QStringLiteral("&About LinCUPL..."), this, &MainWindow::actionAbout);

    // Toolbar: the buttons of the original program in its order, plus the
    // device and option dialogs, LinSim and the error help.
    const QList<QAction *> toolbarActions = {
        actNewWiz, actNewSi, actOpen, actSave, nullptr,
        actPrint, nullptr,
        actCut, actCopy, actPaste, nullptr,
        actDevices, actCompOpts, nullptr,
        actCompDep, actCompIndep, actSimDep, actSimIndep, actLinSim, nullptr,
        actErrHelp
    };
    for (QAction *a : toolbarActions) {
        if (a == nullptr) {
            tb->addSeparator();
            continue;
        }
        tb->addAction(a);
        // Name and shortcut in the tooltip, as there is no button text.
        QString tip = a->iconText();
        if (!a->shortcut().isEmpty()) {
            tip += QStringLiteral(" (%1)").arg(a->shortcut().toString(QKeySequence::NativeText));
        }
        a->setToolTip(tip);
    }
}

bool MainWindow::isCompanionOutputExtension(const QString &filePath) {
    const QString ext = QFileInfo(filePath).suffix().toLower();
    static const QStringList kReadOnlyExts = {
        QStringLiteral("doc"), QStringLiteral("jed"), QStringLiteral("lst"),
        QStringLiteral("abs"), QStringLiteral("pla"), QStringLiteral("so"),
        QStringLiteral("wo"),  QStringLiteral("mx"),  QStringLiteral("pds"),
        QStringLiteral("edf"), QStringLiteral("pdf"), QStringLiteral("sim")
    };
    return kReadOnlyExts.contains(ext);
}

// A file is shown as binary when it holds a NUL byte in its first 8 KiB.
static bool looksBinary(const QByteArray &bytes) {
    return bytes.left(8192).contains('\0');
}

// Hex dump of a binary file: offset, 16 bytes in hex, the same bytes as
// ASCII.  The first lines say what the file is.
static QString hexDumpText(const QString &path, const QByteArray &bytes) {
    static const qsizetype kMaxBytes = 1 << 20;
    const QFileInfo fi(path);
    QString out;
    out += QStringLiteral("Binary file %1, %2 bytes, shown as hex.\n").arg(fi.fileName()).arg(bytes.size());
    if (fi.suffix().compare(QStringLiteral("abs"), Qt::CaseInsensitive) == 0) {
        out += QStringLiteral("This is the compiled design that csim simulates. Its signals, fields and\n"
                              "equations are listed as text in %1.sim, which cupl writes with it.\n")
                   .arg(fi.completeBaseName());
    }
    out += QLatin1Char('\n');
    const qsizetype n = qMin(bytes.size(), kMaxBytes);
    for (qsizetype off = 0; off < n; off += 16) {
        QString hex;
        QString ascii;
        for (qsizetype i = 0; i < 16; ++i) {
            if (off + i < n) {
                const auto c = static_cast<unsigned char>(bytes.at(off + i));
                hex += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'));
                ascii += QLatin1Char((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.');
            } else {
                hex += QStringLiteral("   ");
            }
            if (i == 7) {
                hex += QLatin1Char(' ');
            }
        }
        out += QStringLiteral("%1  %2 |%3|\n").arg(off, 8, 16, QLatin1Char('0')).arg(hex, ascii);
    }
    if (bytes.size() > n) {
        out += QStringLiteral("... (%1 more bytes not shown)\n").arg(bytes.size() - n);
    }
    return out;
}

static bool isAbsFile(const QString &path) {
    return QFileInfo(path).suffix().compare(QStringLiteral("abs"), Qt::CaseInsensitive) == 0;
}

// The absolute file decoded (src/shared/absdump.c): header, symbols,
// product terms and fields.  With the device library the text also says
// which outputs csim complements.  Empty if the decoder is not built in.
static QString absDecodedText(const QString &path, qsizetype size, const QString &libPath) {
#ifdef LINCUPL_HAS_ABSDUMP
    const QByteArray lib = QFile::encodeName(libPath);
    char *decoded = absdump_file(QFile::encodeName(path).constData(), lib.isEmpty() ? nullptr : lib.constData(),
                                 nullptr);
    if (decoded == nullptr) {
        return QString();
    }
    const QFileInfo fi(path);
    QString out = QStringLiteral("Absolute file %1, %2 bytes, decoded.\n"
                                 "This is the compiled design that csim simulates. Its signals, fields and\n"
                                 "equations are listed as text in %3.sim, which cupl writes with it.\n"
                                 "The Raw button (top right) shows the bytes as hex.\n\n")
                      .arg(fi.fileName())
                      .arg(size)
                      .arg(fi.completeBaseName());
    out += QString::fromLatin1(decoded);
    free(decoded);
    return out;
#else
    Q_UNUSED(path);
    Q_UNUSED(size);
    Q_UNUSED(libPath);
    return QString();
#endif
}

CodeEditor *MainWindow::currentEditor() const {
    return qobject_cast<CodeEditor *>(m_tabWidget->currentWidget());
}

CodeEditor *MainWindow::editorAt(int index) const {
    return qobject_cast<CodeEditor *>(m_tabWidget->widget(index));
}

int MainWindow::tabCount() const {
    return m_tabWidget->count();
}

CodeEditor *MainWindow::editorForPath(const QString &filePath) const {
    if (filePath.isEmpty()) {
        return nullptr;
    }
    const QFileInfo targetFi(filePath);
    const QString targetCanon = targetFi.exists() ? targetFi.canonicalFilePath()
                                                  : QDir::cleanPath(targetFi.absoluteFilePath());
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        CodeEditor *ed = editorAt(i);
        if (!ed || ed->filePath().isEmpty()) {
            continue;
        }
        const QFileInfo edFi(ed->filePath());
        const QString edCanon = edFi.exists() ? edFi.canonicalFilePath()
                                              : QDir::cleanPath(edFi.absoluteFilePath());
        if (edCanon.compare(targetCanon, Qt::CaseInsensitive) == 0 || edCanon == targetCanon) {
            return ed;
        }
    }
    return nullptr;
}

bool MainWindow::openFile(const QString &filePath, bool activateTab) {
    if (filePath.isEmpty()) {
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QString absPath = QFileInfo(filePath).canonicalFilePath().isEmpty()
                                ? QFileInfo(filePath).absoluteFilePath()
                                : QFileInfo(filePath).canonicalFilePath();
    const QByteArray bytes = file.readAll();
    const bool binary = looksBinary(bytes);
    // an .abs file is shown decoded, with its hex dump behind the Raw button
    QString decoded;
    if (isAbsFile(absPath)) {
        decoded = absDecodedText(absPath, bytes.size(),
                                 m_deviceLib.libraryPath().isEmpty() ? ToolPaths::activeDeviceLibrary()
                                                                     : m_deviceLib.libraryPath());
    }
    const QString text = (binary || !decoded.isEmpty())
                             ? hexDumpText(absPath, bytes)
                             : ErrorCatalog::normalizeConsoleText(QString::fromLatin1(bytes));
    // binary files are always read-only, so the hex view is never saved
    const bool companion = binary || !decoded.isEmpty() || isCompanionOutputExtension(absPath);

    if (CodeEditor *existing = editorForPath(absPath)) {
        const int idx = m_tabWidget->indexOf(existing);
        if (!decoded.isEmpty()) {
            existing->setAlternateViews(decoded, text);
        } else if (companion || !existing->document()->isModified()) {
            existing->setPlainText(text);
            existing->document()->setModified(false);
        }
        updateTabTitle(idx);
        if (activateTab) {
            m_tabWidget->setCurrentIndex(idx);
        }
        applyDiagnosticsToEditors();
        updateCompanionSidebar();
        return true;
    }

    auto *editor = new CodeEditor(m_tabWidget);
    editor->setFilePath(absPath);
    editor->setCompanionOutput(companion);
    if (!decoded.isEmpty()) {
        editor->setAlternateViews(decoded, text);
    } else {
        editor->setPlainText(text);
    }
    editor->document()->setModified(false);

    connect(editor->document(), &QTextDocument::modificationChanged, this, [this, editor](bool) {
        const int idx = m_tabWidget->indexOf(editor);
        if (idx >= 0) {
            updateTabTitle(idx);
            updateWindowTitleAndStatus();
        }
    });
    connect(editor, &CodeEditor::cursorPositionChanged, this, &MainWindow::updateWindowTitleAndStatus);

    const int newIdx = m_tabWidget->addTab(editor, QFileInfo(absPath).fileName());
    updateTabTitle(newIdx);
    if (activateTab) {
        m_tabWidget->setCurrentIndex(newIdx);
    }

    if (!companion) {
        addRecentFile(absPath);
    }
    applyDiagnosticsToEditors();
    updateCompanionSidebar();
    updateWindowTitleAndStatus();
    return true;
}

void MainWindow::createNewTabWithContent(const QString &titleHint,
                                         const QString &content,
                                         const QString &savePath) {
    if (!savePath.isEmpty()) {
        QSaveFile sf(savePath);
        if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            sf.write(content.toLatin1());
            if (sf.commit()) {
                openFile(savePath, true);
                return;
            }
        }
    }

    auto *editor = new CodeEditor(m_tabWidget);
    editor->setCompanionOutput(false);
    editor->setHighlightLanguage(CuplHighlighter::Language::Pld);
    editor->setPlainText(content);
    editor->document()->setModified(true);
    editor->setProperty("untitledName", titleHint.isEmpty() ? QStringLiteral("Untitled.pld") : titleHint);

    connect(editor->document(), &QTextDocument::modificationChanged, this, [this, editor](bool) {
        const int idx = m_tabWidget->indexOf(editor);
        if (idx >= 0) {
            updateTabTitle(idx);
            updateWindowTitleAndStatus();
        }
    });
    connect(editor, &CodeEditor::cursorPositionChanged, this, &MainWindow::updateWindowTitleAndStatus);

    const int idx = m_tabWidget->addTab(editor, titleHint);
    updateTabTitle(idx);
    m_tabWidget->setCurrentIndex(idx);
    updateWindowTitleAndStatus();
}

void MainWindow::updateTabTitle(int index) {
    CodeEditor *ed = editorAt(index);
    if (!ed) {
        return;
    }
    QString base = ed->filePath().isEmpty()
                       ? ed->property("untitledName").toString()
                       : QFileInfo(ed->filePath()).fileName();
    if (base.isEmpty()) {
        base = QStringLiteral("Untitled.pld");
    }
    if (ed->isCompanionOutput()) {
        base += QStringLiteral(" [RO]");
    } else if (ed->document()->isModified()) {
        base += QStringLiteral(" *");
    }
    m_tabWidget->setTabText(index, base);
    m_tabWidget->setTabToolTip(index, ed->filePath().isEmpty() ? base : ed->filePath());
}

bool MainWindow::saveTab(int tabIndex, bool saveAs) {
    CodeEditor *ed = editorAt(tabIndex);
    if (!ed || ed->isCompanionOutput()) {
        return false;
    }

    QString targetPath = ed->filePath();
    if (saveAs || targetPath.isEmpty()) {
        const QString defaultName = targetPath.isEmpty()
                                        ? ed->property("untitledName").toString()
                                        : targetPath;
        targetPath = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save CUPL File"), defaultName,
            QStringLiteral("CUPL Files (*.pld *.PLD *.si *.SI *.m *.mac);;All Files (*)"));
        if (targetPath.isEmpty()) {
            return false;
        }
    }

    QSaveFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, QStringLiteral("Save Failed"),
                              QStringLiteral("Could not write file:\n%1").arg(targetPath));
        return false;
    }
    out.write(ed->toPlainText().toLatin1());
    if (!out.commit()) {
        QMessageBox::critical(this, QStringLiteral("Save Failed"),
                              QStringLiteral("Failed to commit file:\n%1").arg(targetPath));
        return false;
    }

    const QString canon = QFileInfo(targetPath).canonicalFilePath();
    ed->setFilePath(canon.isEmpty() ? targetPath : canon);
    ed->document()->setModified(false);
    updateTabTitle(tabIndex);
    addRecentFile(ed->filePath());
    updateCompanionSidebar();
    updateWindowTitleAndStatus();
    return true;
}

bool MainWindow::saveCurrentTab() {
    const int idx = m_tabWidget->currentIndex();
    return idx >= 0 ? saveTab(idx, false) : false;
}

bool MainWindow::maybeSaveAllBeforeClose() {
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        CodeEditor *ed = editorAt(i);
        if (!ed || ed->isCompanionOutput() || !ed->document()->isModified()) {
            continue;
        }
        m_tabWidget->setCurrentIndex(i);
        const QString name = m_tabWidget->tabText(i);
        const auto btn = QMessageBox::question(
            this, QStringLiteral("Unsaved Changes"),
            QStringLiteral("Save changes to '%1' before closing?").arg(name),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (btn == QMessageBox::Cancel) {
            return false;
        }
        if (btn == QMessageBox::Save && !saveTab(i, false)) {
            return false;
        }
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!maybeSaveAllBeforeClose()) {
        event->ignore();
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState());
    event->accept();
}

void MainWindow::closeTabAt(int index) {
    CodeEditor *ed = editorAt(index);
    if (!ed) {
        return;
    }
    if (!ed->isCompanionOutput() && ed->document()->isModified()) {
        const auto btn = QMessageBox::question(
            this, QStringLiteral("Unsaved Changes"),
            QStringLiteral("Save changes to '%1'?").arg(m_tabWidget->tabText(index)),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (btn == QMessageBox::Cancel) {
            return;
        }
        if (btn == QMessageBox::Save && !saveTab(index, false)) {
            return;
        }
    }
    QWidget *w = m_tabWidget->widget(index);
    m_tabWidget->removeTab(index);
    w->deleteLater();
    updateCompanionSidebar();
    updateWindowTitleAndStatus();
}

void MainWindow::actionCloseCurrentTab() {
    if (m_tabWidget->currentIndex() >= 0) {
        closeTabAt(m_tabWidget->currentIndex());
    }
}

void MainWindow::actionNewEmptyPld() {
    NewDesignSpec s;
    createNewTabWithContent(QStringLiteral("Untitled.pld"), NewDesignDialog::generatePldTemplate(s));
}

void MainWindow::actionNewSimulationWizard() {
    QString pldText;
    QString suggestedSi;

    const QString pldPath = activeDesignPldPath();
    if (!pldPath.isEmpty()) {
        if (CodeEditor *pldEd = editorForPath(pldPath)) {
            pldText = pldEd->toPlainText();
        } else {
            QFile f(pldPath);
            if (f.open(QIODevice::ReadOnly)) {
                pldText = QString::fromLatin1(f.readAll());
            }
        }
        const QFileInfo fi(pldPath);
        suggestedSi = QDir(fi.absolutePath()).filePath(fi.completeBaseName() + QStringLiteral(".si"));
    } else if (CodeEditor *cur = currentEditor()) {
        pldText = cur->toPlainText();
    }

    NewSimDialog dlg(pldText, suggestedSi, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    createNewTabWithContent(
        dlg.siFilePath().isEmpty() ? QStringLiteral("Untitled.si") : QFileInfo(dlg.siFilePath()).fileName(),
        dlg.generatedSiText(),
        dlg.siFilePath());
}

void MainWindow::actionOpenFile() {
    QString startDir = QDir::currentPath();
    if (CodeEditor *ed = currentEditor(); ed && !ed->filePath().isEmpty()) {
        startDir = QFileInfo(ed->filePath()).absolutePath();
    } else {
        const QString ex = ToolPaths::findExamplesDir();
        if (!ex.isEmpty()) startDir = ex;
    }

    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Open CUPL / Simulation File"), startDir,
        QStringLiteral("CUPL & Simulation Files (*.pld *.PLD *.si *.SI *.m *.mac *.doc *.jed *.lst *.so *.pla *.abs *.sim);;All Files (*)"));
    for (const QString &f : files) {
        openFile(f, true);
    }
}

void MainWindow::actionReloadFromDisk() {
    CodeEditor *ed = currentEditor();
    if (!ed || ed->filePath().isEmpty()) {
        return;
    }
    if (!ed->isCompanionOutput() && ed->document()->isModified()) {
        if (QMessageBox::question(this, QStringLiteral("Reload from Disk"),
                                  QStringLiteral("Discard unsaved edits and reload '%1' from disk?")
                                      .arg(QFileInfo(ed->filePath()).fileName())) != QMessageBox::Yes) {
            return;
        }
    }
    ed->document()->setModified(false);
    openFile(ed->filePath(), true);
}

void MainWindow::actionSave() {
    saveCurrentTab();
}

void MainWindow::actionSaveAs() {
    const int idx = m_tabWidget->currentIndex();
    if (idx >= 0) {
        saveTab(idx, true);
    }
}

void MainWindow::actionSaveAll() {
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        CodeEditor *ed = editorAt(i);
        if (ed && !ed->isCompanionOutput() && ed->document()->isModified()) {
            saveTab(i, false);
        }
    }
}

void MainWindow::actionPrint() {
    CodeEditor *ed = currentEditor();
    if (!ed) {
        return;
    }
#ifdef LINCUPL_HAS_PRINTSUPPORT
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dlg(&printer, this);
    if (dlg.exec() == QDialog::Accepted) {
        ed->print(&printer);
    }
#else
    QMessageBox::information(this, QStringLiteral("Print"),
                             QStringLiteral("Qt6::PrintSupport was not enabled in this build."));
#endif
}

void MainWindow::actionFind() {
    QString seed;
    if (auto *ed = currentEditor()) {
        seed = ed->textCursor().selectedText();
    }
    m_findDialog->focusFind(seed, false);
}

void MainWindow::actionReplace() {
    QString seed;
    if (auto *ed = currentEditor()) {
        seed = ed->textCursor().selectedText();
    }
    m_findDialog->focusFind(seed, true);
}

void MainWindow::actionGotoLine() {
    CodeEditor *ed = currentEditor();
    if (!ed) return;
    bool ok = false;
    const int curLine = ed->textCursor().blockNumber() + 1;
    const int line = QInputDialog::getInt(
        this, QStringLiteral("Go to Line"), QStringLiteral("Line number:"),
        curLine, 1, qMax(1, ed->blockCount()), 1, &ok);
    if (ok) {
        ed->gotoLineColumn(line, 1);
    }
}

void MainWindow::actionInsertConstruct() {
    CodeEditor *ed = currentEditor();
    if (!ed || ed->isReadOnly()) {
        return;
    }
    const QStringList items = {
        QStringLiteral("State Machine (SEQUENCED)"),
        QStringLiteral("Truth Table (TABLE)"),
        QStringLiteral("Macro Definition ($MACRO)"),
        QStringLiteral("Repeat Block ($REPEAT)")
    };
    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, QStringLiteral("Insert CUPL Construct"),
        QStringLiteral("Select template snippet:"), items, 0, false, &ok);
    if (!ok) return;

    QString snippet;
    if (choice.startsWith(QStringLiteral("State Machine"))) {
        snippet = QStringLiteral(
            "$define S0 'b'00\n"
            "$define S1 'b'01\n\n"
            "Sequenced state_field {\n"
            "    present S0  if go next S1;\n"
            "                default next S0;\n"
            "    present S1  next S0;\n"
            "}\n");
    } else if (choice.startsWith(QStringLiteral("Truth Table"))) {
        snippet = QStringLiteral(
            "Table in_field => out_field {\n"
            "    'b'00 => 'b'01;\n"
            "    'b'01 => 'b'10;\n"
            "    'b'10 => 'b'11;\n"
            "    'b'11 => 'b'00;\n"
            "}\n");
    } else if (choice.startsWith(QStringLiteral("Macro"))) {
        snippet = QStringLiteral(
            "$MACRO mux2 sel a b y\n"
            "y = (!sel & a) # (sel & b);\n"
            "$MEND\n"
            "/* usage: mux2( SEL, IN0, IN1, OUT ); */\n");
    } else {
        snippet = QStringLiteral(
            "$repeat i = [0..3]\n"
            "    out{i} = in{i};\n"
            "$repend\n");
    }
    ed->insertPlainText(snippet);
}

void MainWindow::actionTableEditor() {
    CodeEditor *ed = currentEditor();
    if (!ed || ed->isReadOnly()) {
        QMessageBox::information(this, QStringLiteral("Binary Table Editor"),
                                 QStringLiteral("Open an editable .PLD file to insert a truth table."));
        return;
    }
    TableEditorDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        ed->insertPlainText(dlg.generatedTable());
    }
}

void MainWindow::runMacroDialog(int mode) {
    CodeEditor *ed = currentEditor();
    if (!ed || ed->isReadOnly()) {
        QMessageBox::information(this, QStringLiteral("CUPL Macros"),
                                 QStringLiteral("Open an editable .PLD file first."));
        return;
    }
    const QString baseDir = ed->filePath().isEmpty() ? QDir::currentPath()
                                                     : QFileInfo(ed->filePath()).absolutePath();
    MacroDialog dlg(static_cast<MacroDialog::Mode>(mode), ed->toPlainText(), baseDir, this);
    if (dlg.exec() == QDialog::Accepted) {
        const QString text = dlg.generatedText();
        if (!text.isEmpty()) {
            ed->insertPlainText(text);
        }
        if (!dlg.writtenLibraryPath().isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Macro appended to %1").arg(dlg.writtenLibraryPath()), 5000);
        }
    }
}

QString MainWindow::extractPldDeviceMnemonic(const QString &text) const {
    static const QRegularExpression devRe(
        QStringLiteral("\\bdevice\\s+([A-Za-z0-9_+-]+)\\s*;"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = devRe.match(text);
    return m.hasMatch() ? m.captured(1).trimmed().toLower() : QString();
}

QString MainWindow::extractPldHeaderName(const QString &text) const {
    static const QRegularExpression nameRe(
        QStringLiteral("\\bname\\s+([^;\\r\\n]+)\\s*;"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = nameRe.match(text);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

bool MainWindow::updateActivePldDevice(const QString &mnemonic) {
    if (mnemonic.isEmpty()) {
        return false;
    }
    CodeEditor *targetEd = nullptr;
    if (CodeEditor *cur = currentEditor()) {
        if (!cur->isReadOnly() &&
            (cur->filePath().isEmpty() ||
             cur->filePath().endsWith(QStringLiteral(".pld"), Qt::CaseInsensitive) ||
             cur->filePath().endsWith(QStringLiteral(".si"), Qt::CaseInsensitive))) {
            targetEd = cur;
        }
    }
    if (!targetEd) {
        const QString pldPath = activeDesignPldPath();
        targetEd = editorForPath(pldPath);
    }
    if (!targetEd || targetEd->isReadOnly()) {
        return false;
    }

    const QString content = targetEd->toPlainText();
    static const QRegularExpression devStmtRe(
        QStringLiteral("(\\bDevice\\s+)([^;\\r\\n]+)(\\s*;)" ),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = devStmtRe.match(content);

    QTextCursor cur(targetEd->document());
    if (m.hasMatch()) {
        cur.setPosition(m.capturedStart(2));
        cur.setPosition(m.capturedEnd(2), QTextCursor::KeepAnchor);
        cur.insertText(mnemonic);
    } else {
        cur.movePosition(QTextCursor::Start);
        cur.insertText(QStringLiteral("Device   %1;\n").arg(mnemonic));
    }
    updateWindowTitleAndStatus();
    return true;
}

void MainWindow::actionSelectDevice() {
    QString initialDev = m_deviceOverride;
    bool hasEditablePld = false;
    if (CodeEditor *ed = currentEditor()) {
        if (!ed->isReadOnly()) {
            hasEditablePld = true;
            const QString inFile = extractPldDeviceMnemonic(ed->toPlainText());
            if (!inFile.isEmpty()) {
                initialDev = inFile;
            }
        }
    }
    if (initialDev.isEmpty()) {
        initialDev = QStringLiteral("g16v8a");
    }

    DeviceBrowserDialog dlg(&m_deviceLib, initialDev, hasEditablePld, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    if (!dlg.selectedLibraryPath().isEmpty()) {
        ToolPaths::setActiveDeviceLibrary(dlg.selectedLibraryPath());
    }
    if (dlg.shouldUpdatePldHeader()) {
        updateActivePldDevice(dlg.selectedMnemonic());
    }
    if (dlg.shouldSetCompilerOverride()) {
        m_deviceOverride = dlg.selectedMnemonic();
    } else {
        m_deviceOverride.clear();
    }
    rememberProjectDeviceOverride();
    updateWindowTitleAndStatus();
}

void MainWindow::actionCompilerOptions() {
    CompilerOptionsDialog dlg(m_compilerOpts, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_compilerOpts = dlg.options();
        m_compilerOpts.saveToSettings();
    }
}

void MainWindow::actionSimulatorOptions() {
    SimulatorOptionsDialog dlg(m_simulatorOpts, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_simulatorOpts = dlg.options();
        m_simulatorOpts.saveToSettings();
    }
}

void MainWindow::actionEnvironmentPaths() {
    EnvironmentPathsDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        ToolPaths::setCustomBinDir(dlg.customBinDir());
        if (!dlg.selectedLibraryPath().isEmpty()) {
            ToolPaths::setActiveDeviceLibrary(dlg.selectedLibraryPath());
            m_deviceLib.loadLibrary(dlg.selectedLibraryPath());
            const QString cat = ToolPaths::findAtmelCat();
            if (!cat.isEmpty()) m_deviceLib.loadCategories(cat);
        }
        updateWindowTitleAndStatus();
    }
}

QString MainWindow::activeDesignPldPath() const {
    if (!m_projectPld.isEmpty()) {
        return m_projectPld;
    }
    if (CodeEditor *cur = currentEditor()) {
        if (!cur->filePath().isEmpty()) {
            if (cur->filePath().endsWith(QStringLiteral(".pld"), Qt::CaseInsensitive)) {
                return cur->filePath();
            }
            const QString siblingPld = findCompanionFile(cur->filePath(), QStringLiteral("pld"));
            if (!siblingPld.isEmpty()) {
                return siblingPld;
            }
        }
    }
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        CodeEditor *ed = editorAt(i);
        if (ed && ed->filePath().endsWith(QStringLiteral(".pld"), Qt::CaseInsensitive)) {
            return ed->filePath();
        }
    }
    return QString();
}

QString MainWindow::activeDesignSiPath() const {
    if (!m_projectPld.isEmpty()) {
        return findCompanionFile(m_projectPld, QStringLiteral("si"));
    }
    if (CodeEditor *cur = currentEditor()) {
        if (!cur->filePath().isEmpty()) {
            if (cur->filePath().endsWith(QStringLiteral(".si"), Qt::CaseInsensitive)) {
                return cur->filePath();
            }
            const QString siblingSi = findCompanionFile(cur->filePath(), QStringLiteral("si"));
            if (!siblingSi.isEmpty()) {
                return siblingSi;
            }
        }
    }
    const QString pld = activeDesignPldPath();
    if (!pld.isEmpty()) {
        return findCompanionFile(pld, QStringLiteral("si"));
    }
    return QString();
}

QString MainWindow::findCompanionFile(const QString &baseSourcePath, const QString &extWithoutDot) const {
    if (baseSourcePath.isEmpty()) {
        return QString();
    }
    const QFileInfo fi(baseSourcePath);
    const QDir dir = fi.dir();
    if (!dir.exists()) {
        return QString();
    }

    QStringList candidateStems;
    candidateStems.append(fi.completeBaseName());

    // Also check Header "Name <design>;" because CUPL names .jed after Header Name unless -n is passed
    if (CodeEditor *ed = editorForPath(baseSourcePath)) {
        const QString hdrName = extractPldHeaderName(ed->toPlainText());
        if (!hdrName.isEmpty() && !candidateStems.contains(hdrName, Qt::CaseInsensitive)) {
            candidateStems.append(hdrName);
        }
    } else if (fi.exists() && fi.suffix().compare(QStringLiteral("pld"), Qt::CaseInsensitive) == 0) {
        QFile f(baseSourcePath);
        if (f.open(QIODevice::ReadOnly)) {
            const QString hdrName = extractPldHeaderName(QString::fromLatin1(f.read(2048)));
            if (!hdrName.isEmpty() && !candidateStems.contains(hdrName, Qt::CaseInsensitive)) {
                candidateStems.append(hdrName);
            }
        }
    }

    QFileInfo bestMatch;
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (entry.suffix().compare(extWithoutDot, Qt::CaseInsensitive) != 0) {
            continue;
        }
        for (const QString &stem : candidateStems) {
            if (entry.completeBaseName().compare(stem, Qt::CaseInsensitive) == 0) {
                if (!bestMatch.exists() || entry.lastModified() >= bestMatch.lastModified()) {
                    bestMatch = entry;
                }
            }
        }
    }
    return bestMatch.exists() ? bestMatch.canonicalFilePath() : QString();
}

bool MainWindow::ensureActiveSourceSaved(const QStringList &allowedExts, QString *resolvedPathOut) {
    CodeEditor *cur = currentEditor();
    if (cur && !cur->isCompanionOutput()) {
        if (cur->filePath().isEmpty()) {
            if (!saveCurrentTab()) {
                return false;
            }
        } else if (cur->document()->isModified()) {
            if (!saveCurrentTab()) {
                return false;
            }
        }
    }

    for (const QString &ext : allowedExts) {
        if (ext == QStringLiteral("pld")) {
            const QString pld = activeDesignPldPath();
            if (!pld.isEmpty()) {
                if (CodeEditor *pldEd = editorForPath(pld)) {
                    if (pldEd->document()->isModified()) {
                        saveTab(m_tabWidget->indexOf(pldEd), false);
                    }
                }
                *resolvedPathOut = pld;
                return true;
            }
        } else if (ext == QStringLiteral("si")) {
            const QString si = activeDesignSiPath();
            if (!si.isEmpty()) {
                if (CodeEditor *siEd = editorForPath(si)) {
                    if (siEd->document()->isModified()) {
                        saveTab(m_tabWidget->indexOf(siEd), false);
                    }
                }
                *resolvedPathOut = si;
                return true;
            }
        }
    }
    return false;
}

void MainWindow::startCompile(bool deviceIndependent) {
    if (!confirmCompileTarget()) {
        if (m_activeJob == JobKind::CompileThenSimulate) {
            m_activeJob = JobKind::None;
        }
        return;
    }
    QString pldPath;
    if (!ensureActiveSourceSaved({QStringLiteral("pld")}, &pldPath)) {
        QMessageBox::information(this, QStringLiteral("No .PLD File Open"),
                                 QStringLiteral("Please open or save a .PLD design file before compiling."));
        return;
    }

    const QString cuplExe = ToolPaths::findExecutable(QStringLiteral("cupl"));
    if (cuplExe.isEmpty()) {
        m_lastRunOk = false;
        m_lastExitCode = 127;
        m_buildLogEdit->appendPlainText(QStringLiteral("[ERROR] Could not find sibling 'cupl' executable."));
        return;
    }

    QStringList args = m_compilerOpts.buildFlagArgs(deviceIndependent);
    const QString libPath = m_deviceLib.libraryPath().isEmpty()
                                ? ToolPaths::activeDeviceLibrary()
                                : m_deviceLib.libraryPath();
    if (!libPath.isEmpty()) {
        args << QStringLiteral("-u") << libPath;
    }
    if (deviceIndependent) {
        args << QStringLiteral("virtual");
    } else if (!m_deviceOverride.isEmpty()) {
        args << m_deviceOverride;
    }
    args << QFileInfo(pldPath).fileName();

    m_activeJobDeviceIndependent = deviceIndependent;
    const JobKind kind = (m_activeJob == JobKind::CompileThenSimulate)
                             ? JobKind::CompileThenSimulate
                             : JobKind::Compile;
    runToolCommand(kind, cuplExe, args, QFileInfo(pldPath).absolutePath(), pldPath);
}

void MainWindow::startSimulate(bool deviceIndependent) {
    QString siPath;
    if (!ensureActiveSourceSaved({QStringLiteral("si")}, &siPath)) {
        QMessageBox::information(
            this, QStringLiteral("No .SI Simulation File Found"),
            QStringLiteral("No companion .SI file found for this design.\nUse File -> New Simulation (.SI)... to scaffold one."));
        return;
    }

    const QString csimExe = ToolPaths::findExecutable(QStringLiteral("csim"));
    if (csimExe.isEmpty()) {
        m_lastRunOk = false;
        m_lastExitCode = 127;
        m_buildLogEdit->appendPlainText(QStringLiteral("[ERROR] Could not find sibling 'csim' executable."));
        return;
    }

    QStringList args = m_simulatorOpts.buildFlagArgs(deviceIndependent);
    const QString libPath = m_deviceLib.libraryPath().isEmpty()
                                ? ToolPaths::activeDeviceLibrary()
                                : m_deviceLib.libraryPath();
    if (!libPath.isEmpty()) {
        args << QStringLiteral("-u") << libPath;
    }
    if (deviceIndependent) {
        args << QStringLiteral("virtual");
    } else if (!m_deviceOverride.isEmpty()) {
        args << m_deviceOverride;
    }
    // Pass design stem so both csima and vsima resolve <stem>.abs and <stem>.si cleanly
    args << QFileInfo(siPath).completeBaseName();

    m_activeJobDeviceIndependent = deviceIndependent;
    runToolCommand(JobKind::Simulate, csimExe, args, QFileInfo(siPath).absolutePath(), siPath);
}

void MainWindow::runToolCommand(JobKind kind,
                                const QString &program,
                                const QStringList &args,
                                const QString &workingDir,
                                const QString &primarySourcePath) {
    if (m_process->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QStringLiteral("Tool Busy"),
                                 QStringLiteral("A compiler/simulator process is already running."));
        return;
    }

    m_activeJob = kind;
    m_activeJobSourcePath = primarySourcePath;
    m_accumulatedOutput.clear();
    m_diagnostics.clear();
    m_diagTree->clear();
    m_errorExplainBrowser->clear();
    m_bottomTabs->setTabText(0, QStringLiteral("Diagnostics (0)"));
    m_bottomTabs->setCurrentWidget(m_buildLogEdit);
    m_bottomDock->show();

    m_buildLogEdit->clear();
    m_buildLogEdit->appendPlainText(
        QStringLiteral("=== [%1] Running: %2 %3\n=== Working Dir: %4\n")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                 program,
                 args.join(QLatin1Char(' ')),
                 workingDir));

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString libPath = m_deviceLib.libraryPath().isEmpty()
                                ? ToolPaths::activeDeviceLibrary()
                                : m_deviceLib.libraryPath();
    if (!libPath.isEmpty()) {
        env.insert(QStringLiteral("LIBCUPL"), libPath);
    }
    m_process->setProcessEnvironment(env);
    if (!workingDir.isEmpty()) {
        m_process->setWorkingDirectory(workingDir);
    }

    m_jobTimer.start();
    m_process->start(program, args);
}

void MainWindow::onProcessReadyRead() {
    const QByteArray bytes = m_process->readAllStandardOutput();
    if (bytes.isEmpty()) {
        return;
    }
    const QString chunk = ErrorCatalog::normalizeConsoleText(QString::fromLatin1(bytes));
    m_accumulatedOutput.append(chunk);
    m_buildLogEdit->moveCursor(QTextCursor::End);
    m_buildLogEdit->insertPlainText(chunk);
    m_buildLogEdit->moveCursor(QTextCursor::End);
}

void MainWindow::onProcessError(QProcess::ProcessError error) {
    m_lastRunOk = false;
    m_lastExitCode = -1;
    m_buildLogEdit->appendPlainText(
        QStringLiteral("\n[PROCESS ERROR %1] %2\n").arg(static_cast<int>(error)).arg(m_process->errorString()));
}

void MainWindow::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    onProcessReadyRead();

    const qint64 elapsedMs = m_jobTimer.elapsed();
    const JobKind finishedJob = m_activeJob;
    m_activeJob = JobKind::None;

    m_lastExitCode = (exitStatus == QProcess::NormalExit) ? exitCode : -1;
    m_buildLogEdit->appendPlainText(
        QStringLiteral("\n=== Process exited with code %1 (%2 ms) ===").arg(m_lastExitCode).arg(elapsedMs));
    if (finishedJob == JobKind::UserTool) {
        finishUserTool(exitCode, exitStatus);
        return;
    }

    QString sourceText;
    if (!m_activeJobSourcePath.isEmpty()) {
        if (CodeEditor *srcEd = editorForPath(m_activeJobSourcePath)) {
            sourceText = srcEd->toPlainText();
        } else {
            QFile sf(m_activeJobSourcePath);
            if (sf.open(QIODevice::ReadOnly)) {
                sourceText = QString::fromLatin1(sf.readAll());
            }
        }
    }

    QString lstOrSo;
    if (finishedJob == JobKind::Compile || finishedJob == JobKind::CompileThenSimulate) {
        lstOrSo = findCompanionFile(m_activeJobSourcePath, QStringLiteral("lst"));
    } else if (finishedJob == JobKind::Simulate) {
        lstOrSo = findCompanionFile(m_activeJobSourcePath, QStringLiteral("so"));
    }

    m_diagnostics = m_errorCatalog.parseToolOutput(
        m_accumulatedOutput, m_activeJobSourcePath, sourceText, lstOrSo);

    int errOrFatalCount = 0;
    for (const DiagnosticItem &d : m_diagnostics) {
        if (d.severity != QStringLiteral("Warning")) {
            ++errOrFatalCount;
        }
    }
    m_lastRunOk = (m_lastExitCode == 0 && errOrFatalCount == 0);

    // Populate Diagnostics tree
    m_diagTree->clear();
    for (int i = 0; i < m_diagnostics.size(); ++i) {
        const DiagnosticItem &d = m_diagnostics.at(i);
        auto *item = new QTreeWidgetItem(m_diagTree);
        item->setText(0, d.severity);
        item->setText(1, QStringLiteral("[%1]").arg(d.code));
        item->setText(2, d.filePath.isEmpty() ? QString() : QFileInfo(d.filePath).fileName());
        item->setText(3, d.line > 0 ? QString::number(d.line)
                                    : (d.vectorNumber > 0 ? QStringLiteral("Vec %1").arg(d.vectorNumber)
                                                          : QStringLiteral("-")));
        item->setText(4, d.message);
        item->setData(0, Qt::UserRole, i);

        if (d.severity == QStringLiteral("Fatal") || d.severity == QStringLiteral("Error")) {
            item->setForeground(0, QColor(210, 45, 55));
        } else {
            item->setForeground(0, QColor(195, 132, 0));
        }
    }
    m_bottomTabs->setTabText(0, QStringLiteral("Diagnostics (%1)").arg(m_diagnostics.size()));
    applyDiagnosticsToEditors();

    if (!m_diagnostics.isEmpty()) {
        m_bottomTabs->setCurrentIndex(0);
        m_diagTree->setCurrentItem(m_diagTree->topLevelItem(0));
        showErrorExplanation(m_diagnostics.first());
    }

    // Refresh or auto-load companion tabs
    if (finishedJob == JobKind::Compile || finishedJob == JobKind::CompileThenSimulate) {
        refreshCompanionTabs(m_activeJobSourcePath, true);
        if (finishedJob == JobKind::CompileThenSimulate && m_lastRunOk) {
            startSimulate(m_activeJobDeviceIndependent);
            return;
        }
    } else if (finishedJob == JobKind::Simulate) {
        refreshCompanionTabs(m_activeJobSourcePath, true);
        if (m_lastRunOk && m_simulatorOpts.autoOpenLinSim) {
            actionLaunchLinSim();
        }
    } else if (finishedJob == JobKind::Backpin) {
        finishBackpin();
    }

    statusBar()->showMessage(
        m_lastRunOk ? QStringLiteral("Completed successfully (%1 ms).").arg(elapsedMs)
                    : QStringLiteral("Finished with errors (exit code %1).").arg(m_lastExitCode),
        5000);
    updateCompanionSidebar();
}

bool MainWindow::waitForActiveProcess(int timeoutMs) {
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return true;
    }
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(m_process, &QProcess::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    return m_process->state() == QProcess::NotRunning;
}

QString MainWindow::buildLogText() const {
    return m_buildLogEdit ? m_buildLogEdit->toPlainText() : QString();
}

void MainWindow::refreshCompanionTabs(const QString &designSourcePath, bool autoOpenGenerated) {
    if (designSourcePath.isEmpty()) {
        return;
    }
    CodeEditor *previouslyActive = currentEditor();

    const QStringList companionExts = {
        QStringLiteral("doc"),
        QStringLiteral("jed"),
        QStringLiteral("lst"),
        QStringLiteral("abs"),
        QStringLiteral("sim"),
        QStringLiteral("pla"),
        QStringLiteral("so")
    };

    for (const QString &ext : companionExts) {
        const QString found = findCompanionFile(designSourcePath, ext);
        if (found.isEmpty()) {
            continue;
        }
        if (autoOpenGenerated || editorForPath(found) != nullptr) {
            openFile(found, false);
        }
    }

    if (previouslyActive) {
        const int idx = m_tabWidget->indexOf(previouslyActive);
        if (idx >= 0) {
            m_tabWidget->setCurrentIndex(idx);
        }
    }
    updateCompanionSidebar();
}

void MainWindow::updateCompanionSidebar() {
    m_companionTree->clear();
    const QString base = !activeDesignPldPath().isEmpty()
                             ? activeDesignPldPath()
                             : activeDesignSiPath();
    if (base.isEmpty()) {
        return;
    }

    struct CompanionRole {
        QString ext;
        QString role;
    };
    const QList<CompanionRole> specs = {
        {QStringLiteral("pld"), QStringLiteral("CUPL Source")},
        {QStringLiteral("si"),  QStringLiteral("Simulation Vectors")},
        {QStringLiteral("doc"), QStringLiteral("Equations / Doc")},
        {QStringLiteral("jed"), QStringLiteral("JEDEC Fuse Map")},
        {QStringLiteral("lst"), QStringLiteral("Compiler Listing")},
        {QStringLiteral("abs"), QStringLiteral("Absolute File (binary)")},
        {QStringLiteral("sim"), QStringLiteral("Simulator Equations")},
        {QStringLiteral("pla"), QStringLiteral("Berkeley PLA")},
        {QStringLiteral("so"),  QStringLiteral("Simulation Listing")},
        {QStringLiteral("wo"),  QStringLiteral("Waveform Output")}
    };

    for (const CompanionRole &s : specs) {
        const QString found = findCompanionFile(base, s.ext);
        auto *item = new QTreeWidgetItem(m_companionTree);
        if (!found.isEmpty()) {
            const QFileInfo fi(found);
            item->setText(0, fi.fileName());
            item->setText(1, s.role);
            item->setText(2, QStringLiteral("%1 B").arg(fi.size()));
            item->setData(0, Qt::UserRole, found);
        } else {
            item->setText(0, QFileInfo(base).completeBaseName() + QLatin1Char('.') + s.ext);
            item->setText(1, s.role);
            item->setText(2, QStringLiteral("Not generated"));
            item->setForeground(2, QColor(140, 140, 140));
        }
    }
}

void MainWindow::onCompanionItemDoubleClicked(QTreeWidgetItem *item, int /*column*/) {
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) {
        return;
    }
    if (path.endsWith(QStringLiteral(".wo"), Qt::CaseInsensitive)) {
        actionLaunchLinSim();
        return;
    }
    openFile(path, true);
}

void MainWindow::applyDiagnosticsToEditors() {
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (CodeEditor *ed = editorAt(i)) {
            ed->clearDiagnostics();
        }
    }

    QHash<CodeEditor *, QSet<int>> errsByEd;
    QHash<CodeEditor *, QSet<int>> warnsByEd;
    for (const DiagnosticItem &d : m_diagnostics) {
        if (d.line <= 0 || d.filePath.isEmpty()) {
            continue;
        }
        if (CodeEditor *ed = editorForPath(d.filePath)) {
            if (d.severity == QStringLiteral("Warning")) {
                warnsByEd[ed].insert(d.line);
            } else {
                errsByEd[ed].insert(d.line);
            }
        }
    }

    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (CodeEditor *ed = editorAt(i)) {
            if (errsByEd.contains(ed) || warnsByEd.contains(ed)) {
                ed->setDiagnosticLines(errsByEd.value(ed), warnsByEd.value(ed));
            }
        }
    }
}

void MainWindow::showErrorExplanation(const DiagnosticItem &diag) {
    const CuplErrorEntry entry = m_errorCatalog.lookup(diag.lookupCode);
    if (entry.isValid()) {
        m_errorExplainBrowser->setHtml(QStringLiteral(
            "<h4 style='margin:2px 0;'>[%1] %2</h4>"
            "<p style='margin:2px 0;'><b>Severity:</b> %3 &nbsp;|&nbsp; <b>Module:</b> %4</p>"
            "<p style='margin:4px 0;'><b>Reported:</b> <code>%5</code></p>"
            "<hr/><p style='white-space:pre-wrap;margin-top:4px;'>%6</p>")
            .arg(entry.code.toHtmlEscaped(),
                 entry.summary.toHtmlEscaped(),
                 entry.severity.toHtmlEscaped(),
                 entry.moduleName.toHtmlEscaped(),
                 diag.message.toHtmlEscaped(),
                 entry.explanation.toHtmlEscaped()));
    } else {
        m_errorExplainBrowser->setHtml(QStringLiteral(
            "<h4>[%1] %2</h4><p>No additional entry found in CuplErr.txt for code <code>%1</code>.</p>")
            .arg(diag.code.toHtmlEscaped(), diag.message.toHtmlEscaped()));
    }
}

void MainWindow::activateDiagnostic(int index) {
    if (index < 0 || index >= m_diagnostics.size()) {
        return;
    }
    const DiagnosticItem &diag = m_diagnostics.at(index);
    showErrorExplanation(diag);

    QString targetPath = diag.filePath;
    int targetLine = diag.line;

    if ((targetPath.isEmpty() || targetLine <= 0) && !m_activeJobSourcePath.isEmpty()) {
        const QString lst = findCompanionFile(m_activeJobSourcePath, QStringLiteral("lst"));
        if (!lst.isEmpty()) {
            QFile f(lst);
            if (f.open(QIODevice::ReadOnly)) {
                const QStringList lines = ErrorCatalog::normalizeConsoleText(QString::fromLatin1(f.readAll()))
                                              .split(QLatin1Char('\n'));
                const QString tag = QStringLiteral("[%1]").arg(diag.code);
                for (int i = 0; i < lines.size(); ++i) {
                    if (lines.at(i).contains(tag, Qt::CaseInsensitive)) {
                        targetPath = lst;
                        targetLine = i + 1;
                        break;
                    }
                }
            }
        }
    }

    if (!targetPath.isEmpty() && openFile(targetPath, true)) {
        if (CodeEditor *ed = editorForPath(targetPath)) {
            if (targetLine > 0) {
                ed->gotoLineColumn(targetLine, qMax(1, diag.column));
            }
        }
    }
}

void MainWindow::onDiagnosticItemClicked(QTreeWidgetItem *item, int /*column*/) {
    if (!item) return;
    activateDiagnostic(item->data(0, Qt::UserRole).toInt());
}

void MainWindow::onDiagnosticItemDoubleClicked(QTreeWidgetItem *item, int /*column*/) {
    if (!item) return;
    const int idx = item->data(0, Qt::UserRole).toInt();
    activateDiagnostic(idx);
    if (idx >= 0 && idx < m_diagnostics.size()) {
        ErrorViewerDialog dlg(&m_errorCatalog, m_diagnostics.at(idx).lookupCode, this);
        dlg.exec();
    }
}

void MainWindow::actionCompileDependent() {
    startCompile(false);
}

void MainWindow::actionCompileIndependent() {
    startCompile(true);
}

void MainWindow::actionSimulateDependent() {
    startSimulate(false);
}

void MainWindow::actionSimulateIndependent() {
    startSimulate(true);
}

void MainWindow::actionCompileAndSimulate() {
    m_activeJob = JobKind::CompileThenSimulate;
    startCompile(false);
}

void MainWindow::actionLaunchLinSim() {
    const QString linsimExe = ToolPaths::findExecutable(QStringLiteral("linsim"));
    if (linsimExe.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("Waveform Simulator (linsim)"),
            QStringLiteral("Could not locate sibling 'linsim' binary in <appDir>, build/bin, or PATH."));
        return;
    }

    QStringList args;
    const QString base = !activeDesignSiPath().isEmpty() ? activeDesignSiPath() : activeDesignPldPath();
    if (!base.isEmpty()) {
        const QString wo = findCompanionFile(base, QStringLiteral("wo"));
        const QString si = findCompanionFile(base, QStringLiteral("si"));
        if (!wo.isEmpty()) args << wo;
        else if (!si.isEmpty()) args << si;
    }

    const QString workDir = base.isEmpty() ? QDir::currentPath() : QFileInfo(base).absolutePath();
    if (!QProcess::startDetached(linsimExe, args, workDir)) {
        QMessageBox::warning(this, QStringLiteral("Launch Failed"),
                             QStringLiteral("Failed to launch:\n%1").arg(linsimExe));
    } else {
        statusBar()->showMessage(QStringLiteral("Launched linsim: %1").arg(args.join(QLatin1Char(' '))), 4000);
    }
}

void MainWindow::startBackpin(const BackpinRequest &req) {
    const QString backpinExe = ToolPaths::findExecutable(QStringLiteral("backpin"));
    if (backpinExe.isEmpty()) {
        m_lastRunOk = false;
        m_lastExitCode = 127;
        m_buildLogEdit->appendPlainText(QStringLiteral("[ERROR] Could not find sibling 'backpin' binary."));
        return;
    }

    // finishBackpin() checks backpin's result against this copy
    m_backpinSourceBefore.clear();
    m_backpinHaveSource = false;
    QFile before(req.sourcePldPath);
    if (before.open(QIODevice::ReadOnly)) {
        m_backpinSourceBefore = before.readAll();
        m_backpinHaveSource = true;
    }
    runToolCommand(JobKind::Backpin, backpinExe, req.buildArgs(),
                   QFileInfo(req.sourcePldPath).absolutePath(), req.sourcePldPath);
}

void MainWindow::actionRunBackpin() {
    QString pldPath;
    ensureActiveSourceSaved({QStringLiteral("pld")}, &pldPath);

    QString dev = m_deviceOverride;
    if (dev.isEmpty() && !pldPath.isEmpty()) {
        if (CodeEditor *ed = editorForPath(pldPath)) {
            dev = extractPldDeviceMnemonic(ed->toPlainText());
        }
    }

    BackpinDialog dlg(&m_deviceLib, pldPath, dev, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    if (ToolPaths::findExecutable(QStringLiteral("backpin")).isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("backpin Not Found"),
                             QStringLiteral("Could not find sibling 'backpin' binary."));
        return;
    }
    startBackpin(dlg.request());
}

void MainWindow::actionInspectLibraryToc() {
    const QString cbldExe = ToolPaths::findExecutable(QStringLiteral("cbld"));
    const QString libPath = m_deviceLib.libraryPath().isEmpty()
                                ? ToolPaths::activeDeviceLibrary()
                                : m_deviceLib.libraryPath();
    if (cbldExe.isEmpty() || libPath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("cbld"),
                                 QStringLiteral("Ensure 'cbld' and a .dl device library are configured."));
        return;
    }
    runToolCommand(JobKind::CbldToc, cbldExe,
                   {QStringLiteral("-l"), QStringLiteral("-u"), libPath},
                   QDir::currentPath(), QString());
}

void MainWindow::actionErrorMessagesCatalog() {
    QString initialCode;
    if (QTreeWidgetItem *cur = m_diagTree->currentItem()) {
        const int idx = cur->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && idx < m_diagnostics.size()) {
            initialCode = m_diagnostics.at(idx).lookupCode;
        }
    }
    ErrorViewerDialog dlg(&m_errorCatalog, initialCode, this);
    dlg.exec();
}

void MainWindow::actionAbout() {
    QMessageBox::about(
        this, QStringLiteral("About LinCUPL"),
        QStringLiteral(
            "<h3>LinCUPL " LINCUPL_VERSION "</h3>"
            "<p>Native cross-platform Qt6 IDE for CUPL designs.</p>"
            "<p><b>Active Library:</b> %1 (%2 devices)<br/>"
            "<b>Error Catalog:</b> %3 (%4 entries)</p>"
            "<p>Supports GAL16V8, GAL20V8, GAL22V10, ATF750C, ATF2500C, ATF150x, and full CUPL/CSIM workflows.</p>")
            .arg(m_deviceLib.libraryPath().toHtmlEscaped(),
                 QString::number(m_deviceLib.devices().size()),
                 m_errorCatalog.catalogPath().toHtmlEscaped(),
                 QString::number(m_errorCatalog.entries().size())));
}

void MainWindow::onCurrentTabChanged(int /*index*/) {
    updateCompanionSidebar();
    updateWindowTitleAndStatus();
}

void MainWindow::updateWindowTitleAndStatus() {
    CodeEditor *ed = currentEditor();
    QString activeDev = m_deviceOverride;

    if (ed) {
        const QString titleFile = ed->filePath().isEmpty()
                                      ? ed->property("untitledName").toString()
                                      : ed->filePath();
        setWindowTitle(QStringLiteral("%1%2%3 - LinCUPL")
                           .arg(titleFile, ed->document()->isModified() ? QStringLiteral(" *") : QString(),
                                m_projectPld.isEmpty() ? QString()
                                                       : QStringLiteral(" - Project %1").arg(projectName())));

        if (activeDev.isEmpty()) {
            activeDev = extractPldDeviceMnemonic(ed->toPlainText());
        }

        const QTextCursor c = ed->textCursor();
        m_statusCursorLabel->setText(
            QStringLiteral("Ln %1, Col %2%3")
                .arg(c.blockNumber() + 1)
                .arg(c.columnNumber() + 1)
                .arg(ed->isReadOnly() ? QStringLiteral(" [Read-Only]") : QString()));
    } else {
        setWindowTitle(m_projectPld.isEmpty()
                           ? QStringLiteral("LinCUPL - Universal Compiler for Programmable Logic IDE")
                           : QStringLiteral("Project %1 - LinCUPL").arg(projectName()));
        m_statusCursorLabel->setText(QStringLiteral("No file open"));
    }

    m_statusProjectLabel->setText(m_projectPld.isEmpty() ? QStringLiteral("No project")
                                                         : QStringLiteral("Project: %1").arg(projectName()));
    m_statusProjectLabel->setToolTip(m_projectPld);
    m_closeProjectAction->setEnabled(!m_projectPld.isEmpty());
    m_companionDock->setWindowTitle(m_projectPld.isEmpty()
                                        ? QStringLiteral("Design && Output Files")
                                        : QStringLiteral("Project %1: Design && Output Files").arg(projectName()));

    if (activeDev.isEmpty()) {
        m_statusDeviceLabel->setText(QStringLiteral("Device: (from .PLD)"));
    } else {
        const DeviceInfo info = m_deviceLib.findDevice(activeDev);
        if (info.isValid()) {
            m_statusDeviceLabel->setText(
                QStringLiteral("Device: %1 (%2 pins, %3 pterms)%4")
                    .arg(info.mnemonic)
                    .arg(info.pins)
                    .arg(info.productTerms)
                    .arg(m_deviceOverride.isEmpty() ? QString() : QStringLiteral(" [Override]")));
        } else {
            m_statusDeviceLabel->setText(QStringLiteral("Device: %1").arg(activeDev));
        }
    }

    m_statusLibLabel->setText(
        m_deviceLib.libraryPath().isEmpty()
            ? QStringLiteral("Lib: Not found")
            : QStringLiteral("Lib: %1 (%2)").arg(QFileInfo(m_deviceLib.libraryPath()).fileName(),
                                                 m_deviceLib.libraryRevision()));
}

void MainWindow::addRecentFile(const QString &path) {
    if (path.isEmpty()) return;
    QSettings s;
    QStringList files = s.value(QStringLiteral("recentFiles")).toStringList();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > 10) {
        files.removeLast();
    }
    s.setValue(QStringLiteral("recentFiles"), files);
    rebuildRecentFilesMenu();
}

// Recent projects first, then recent files.
void MainWindow::rebuildRecentFilesMenu() {
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    QSettings s;
    const QStringList projects = recentProjects();
    const QStringList files = s.value(QStringLiteral("recentFiles")).toStringList();
    if (files.isEmpty() && projects.isEmpty()) {
        QAction *emptyAct = m_recentMenu->addAction(QStringLiteral("(No Recent Files)"));
        emptyAct->setEnabled(false);
        return;
    }
    for (const QString &p : projects) {
        QAction *act = m_recentMenu->addAction(
            lincuplIcon(QStringLiteral("project")),
            QStringLiteral("Project %1  —  %2").arg(QFileInfo(p).completeBaseName(), p));
        connect(act, &QAction::triggered, this, [this, p]() { openProject(p); });
    }
    if (!projects.isEmpty() && !files.isEmpty()) {
        m_recentMenu->addSeparator();
    }
    for (const QString &f : files) {
        QAction *act = m_recentMenu->addAction(QStringLiteral("%1  —  %2").arg(QFileInfo(f).fileName(), f));
        connect(act, &QAction::triggered, this, [this, f]() { openFile(f, true); });
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(QStringLiteral("Clear Recent Files"), this, [this]() {
        QSettings st;
        st.remove(QStringLiteral("recentFiles"));
        st.remove(QStringLiteral("recentProjects"));
        rebuildRecentFilesMenu();
    });
}

void MainWindow::rebuildWindowMenu() {
    m_windowMenu->clear();
    m_windowMenu->addAction(QStringLiteral("&Next Tab"), QKeySequence::NextChild, this, [this]() {
        if (m_tabWidget->count() > 1) {
            m_tabWidget->setCurrentIndex((m_tabWidget->currentIndex() + 1) % m_tabWidget->count());
        }
    });
    m_windowMenu->addAction(QStringLiteral("&Previous Tab"), QKeySequence::PreviousChild, this, [this]() {
        if (m_tabWidget->count() > 1) {
            m_tabWidget->setCurrentIndex((m_tabWidget->currentIndex() + m_tabWidget->count() - 1) % m_tabWidget->count());
        }
    });
    m_windowMenu->addAction(QStringLiteral("Close All &Read-Only Output Tabs"), this, [this]() {
        for (int i = m_tabWidget->count() - 1; i >= 0; --i) {
            if (CodeEditor *ed = editorAt(i); ed && ed->isCompanionOutput()) {
                closeTabAt(i);
            }
        }
    });
    m_windowMenu->addSeparator();
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        QAction *a = m_windowMenu->addAction(QStringLiteral("&%1 %2").arg(i + 1).arg(m_tabWidget->tabText(i)));
        a->setCheckable(true);
        a->setChecked(i == m_tabWidget->currentIndex());
        connect(a, &QAction::triggered, this, [this, i]() { m_tabWidget->setCurrentIndex(i); });
    }
}
