#include "MainWindow.h"
#include "Dialogs.h"
#include "../common/action_icons.h"
#include "../common/cupl_highlighter.h"
#include "../common/help_window.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>

#ifdef LINSIM_HAS_PRINTSUPPORT
#include <QPrintDialog>
#include <QPrinter>
#endif

namespace {

// All LinSim windows of the process, in the order they were created.
QList<MainWindow *> &windowRegistry() {
    static QList<MainWindow *> registry;
    return registry;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), doc_(SimDocument::createDefaultNew()) {
    windowRegistry().append(this);
    setWindowTitle(QStringLiteral("LinSim — CUPL Digital Waveform Simulator"));
    resize(1240, 780);

    setupUi();
    setupMenusAndToolbar();
    loadSettings();

    waveformEditor_->setDocument(&doc_);
    refreshCompanionTabs();
    refreshSignalDefinitionPane(waveformEditor_->selectedSignalIndex());
    refreshWindowTitle();
}

MainWindow::~MainWindow() {
    windowRegistry().removeAll(this);
}

QList<MainWindow *> MainWindow::windows() {
    return windowRegistry();
}

MainWindow *MainWindow::newWindow(MainWindow *placeNear) {
    auto *win = new MainWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    if (placeNear && placeNear->isVisible() && canArrangeWindows()) {
        // Down and to the right of the window it was opened from, kept on
        // that window's screen.
        const QRect area = placeNear->screen()->availableGeometry();
        QPoint pos = placeNear->frameGeometry().topLeft() + QPoint(32, 32);
        if (pos.x() + win->width() > area.right()) pos.setX(area.left());
        if (pos.y() + win->height() > area.bottom()) pos.setY(area.top());
        win->move(pos);
    }
    win->show();
    return win;
}

MainWindow *MainWindow::openInWindow(const QString &path, MainWindow *origin, QString *errorOut) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (MainWindow *w : windows()) {
        if (!canonical.isEmpty() && !w->doc_.filePath.isEmpty() &&
            QFileInfo(w->doc_.filePath).canonicalFilePath() == canonical) {
            if (w->isMinimized()) w->showNormal();
            w->raise();
            w->activateWindow();
            return w;
        }
    }

    SimDocument loaded;
    if (!loaded.loadFile(path, errorOut)) {
        return nullptr;
    }
    MainWindow *target = (origin && origin->isUntouched()) ? origin : newWindow(origin);
    target->adoptDocument(std::move(loaded));
    target->raise();
    target->activateWindow();
    return target;
}

bool MainWindow::canArrangeWindows() {
    return !QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

void MainWindow::arrangeWindows(bool tile, QScreen *screen) {
    if (!screen) return;
    QList<MainWindow *> wins;
    for (MainWindow *w : windows()) {
        if (w->isVisible()) wins.append(w);
    }
    if (wins.isEmpty()) return;

    const QRect area = screen->availableGeometry();
    // Places a window so that its frame fills frameRect.  A window that
    // cannot become that small (its minimum size) is moved back into the
    // available area instead of reaching past its right or bottom edge.
    auto place = [area](MainWindow *w, const QRect &frameRect) {
        if (w->isMinimized() || w->isMaximized() || w->isFullScreen()) w->showNormal();
        const QRect frame = w->frameGeometry();
        const QRect inner = w->geometry();
        const QSize extra(frame.width() - inner.width(), frame.height() - inner.height());
        w->resize(frameRect.size() - extra);
        const QSize size = w->size() + extra;
        QPoint pos = frameRect.topLeft();
        pos.setX(qMax(area.left(), qMin(pos.x(), area.right() + 1 - size.width())));
        pos.setY(qMax(area.top(), qMin(pos.y(), area.bottom() + 1 - size.height())));
        w->move(pos);
    };

    if (tile) {
        const int n = wins.size();
        int cols = 1;
        while (cols * cols < n) ++cols;
        const int rows = (n + cols - 1) / cols;
        const int cellH = area.height() / rows;
        for (int i = 0; i < n; ++i) {
            const int row = i / cols;
            // The last row may hold fewer windows; they share its width.
            const int inRow = (row == rows - 1) ? n - row * cols : cols;
            const int col = i - row * cols;
            const int cellW = area.width() / inRow;
            place(wins[i], QRect(area.left() + col * cellW, area.top() + row * cellH, cellW, cellH));
        }
    } else {
        // Each window a title bar lower and to the right of the previous one;
        // the active window ends up in front.
        if (auto *active = qobject_cast<MainWindow *>(QApplication::activeWindow());
            active && wins.removeOne(active)) {
            wins.append(active);
        }
        const int titleH = qMax(24, wins.first()->geometry().top() - wins.first()->frameGeometry().top());
        const int step = titleH + 4;
        const QSize size(qMax(area.width() * 3 / 4, qMin(area.width(), 640)),
                         qMax(area.height() * 3 / 4, qMin(area.height(), 480)));
        const int fit = qMax(1, qMin((area.width() - size.width()) / step,
                                     (area.height() - size.height()) / step) + 1);
        for (int i = 0; i < wins.size(); ++i) {
            const int k = i % fit;
            place(wins[i], QRect(area.topLeft() + QPoint(k * step, k * step), size));
            wins[i]->raise();
        }
    }
}

bool MainWindow::saveAll(bool interactive, QString *errorOut) {
    bool allSaved = true;
    int saved = 0;
    QStringList errors;
    // Save dialogs run event loops, so hold the windows by QPointer.
    QList<QPointer<MainWindow>> wins;
    for (MainWindow *w : windows()) wins.append(w);
    for (const QPointer<MainWindow> &w : wins) {
        if (!w || !w->hasUnsavedChanges()) continue;
        bool ok = false;
        if (interactive) {
            // Show the window whose document asks for a file name.
            if (w->doc_.filePath.isEmpty() || w->doc_.loadedFromWo) {
                if (w->isMinimized()) w->showNormal();
                w->raise();
                w->activateWindow();
            }
            ok = w->onFileSave();
        } else {
            QString err;
            ok = w->saveDocument(&err);
            if (!ok) errors.append(QStringLiteral("%1: %2").arg(w->documentDisplayName(), err));
        }
        if (ok) {
            ++saved;
        } else {
            allSaved = false;
        }
    }
    if (errorOut) *errorOut = errors.join(QLatin1Char('\n'));
    if (interactive) {
        if (auto *active = qobject_cast<MainWindow *>(QApplication::activeWindow())) {
            active->statusBar()->showMessage(QStringLiteral("Saved %1 document(s)").arg(saved), 3000);
        }
    }
    return allSaved;
}

bool MainWindow::isUntouched() const {
    return doc_.filePath.isEmpty() && !doc_.modified && !siEditorDirty_ && undoStack_.isEmpty();
}

QString MainWindow::documentDisplayName() const {
    if (!doc_.filePath.isEmpty()) return QFileInfo(doc_.filePath).fileName();
    return doc_.header.name.isEmpty() ? QStringLiteral("Untitled") : doc_.header.name;
}

QString MainWindow::findCliBinary(const QString &toolName) {
#ifdef Q_OS_WIN
    const QString name = toolName + QStringLiteral(".exe");
#else
    const QString name = toolName;
#endif
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        QDir(appDir).filePath(name),
        QDir(appDir).filePath(QStringLiteral("../bin/") + name),
        QDir::current().filePath(QStringLiteral("build/bin/") + name),
        QDir::current().filePath(QStringLiteral("build/") + name),
    };
#ifdef LINSIM_SOURCE_ROOT
    const QString srcRoot = QStringLiteral(LINSIM_SOURCE_ROOT);
    candidates.append(QDir(srcRoot).filePath(QStringLiteral("build/bin/") + name));
    candidates.append(QDir(srcRoot).filePath(QStringLiteral("build/") + name));
#endif

    for (const QString &cand : candidates) {
        QFileInfo fi(cand);
        if (fi.exists() && fi.isFile() && fi.isExecutable()) {
            return fi.canonicalFilePath();
        }
    }
    return QStandardPaths::findExecutable(name);
}

QStringList MainWindow::discoverLibraryCandidates() {
    QStringList dirs;
    const QString envShare = qEnvironmentVariable("LINCUPL_SHARE");
    if (!envShare.isEmpty()) dirs.append(envShare);

    const QString appDir = QCoreApplication::applicationDirPath();
    dirs.append(QDir(appDir).filePath(QStringLiteral("../share/lincupl")));
    dirs.append(QDir(appDir).filePath(QStringLiteral("share/lincupl")));
    dirs.append(QDir(appDir).filePath(QStringLiteral("../Shared")));
    dirs.append(QDir::current().filePath(QStringLiteral("share/lincupl")));
    dirs.append(QDir::current().filePath(QStringLiteral("../share/lincupl")));
#ifdef LINSIM_SOURCE_ROOT
    dirs.append(QDir(QStringLiteral(LINSIM_SOURCE_ROOT)).filePath(QStringLiteral("build/share/lincupl")));
    dirs.append(QDir(QStringLiteral(LINSIM_SOURCE_ROOT)).filePath(QStringLiteral("share/lincupl")));
#endif

    QStringList out;
    const QString envLib = qEnvironmentVariable("LIBCUPL");
    if (!envLib.isEmpty() && QFileInfo::exists(envLib)) {
        out.append(QFileInfo(envLib).canonicalFilePath());
    }

    for (const QString &d : dirs) {
        QDir dir(d);
        if (!dir.exists()) continue;
        for (const QString &fn : {QStringLiteral("lincupl.dl"), QStringLiteral("Atmel.dl"), QStringLiteral("cupl.dl"),
                                  QStringLiteral("atmel.dl"), QStringLiteral("CUPL.DL")}) {
            const QString p = dir.filePath(fn);
            if (QFileInfo::exists(p)) {
                const QString canon = QFileInfo(p).canonicalFilePath();
                if (!out.contains(canon)) out.append(canon);
            }
        }
    }
    return out;
}

QString MainWindow::resolveActiveLibrary() const {
    if (!customLibraryPath_.trimmed().isEmpty() && QFileInfo::exists(customLibraryPath_.trimmed())) {
        return QFileInfo(customLibraryPath_.trimmed()).absoluteFilePath();
    }
    const QStringList cands = discoverLibraryCandidates();
    return cands.isEmpty() ? QString() : cands.first();
}

void MainWindow::setupUi() {
    centralTabs_ = new QTabWidget(this);
    centralTabs_->setDocumentMode(true);
    setCentralWidget(centralTabs_);

    // Tab 0: Waveform Editor + collapsible Signal Definitions Inspector
    waveSplitter_ = new QSplitter(Qt::Vertical, centralTabs_);
    waveformEditor_ = new WaveformEditorWidget(waveSplitter_);

    sigDefView_ = new QPlainTextEdit(waveSplitter_);
    sigDefView_->setReadOnly(true);
    sigDefView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    sigDefView_->setMaximumHeight(160);
    sigDefView_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background-color: #111827; color: #e2e8f0; border-top: 1px solid #334155; }"));
    sigDefView_->hide();

    waveSplitter_->setStretchFactor(0, 5);
    waveSplitter_->setStretchFactor(1, 1);
    centralTabs_->addTab(waveSplitter_, QStringLiteral("Waveform Editor"));

    // Tab 1: Simulation Listing (.SO)
    soListingView_ = new QPlainTextEdit(centralTabs_);
    soListingView_->setReadOnly(true);
    soListingView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    soListingView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    soListingView_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background-color: #0f172a; color: #e2e8f0; }"));
    // Both views always draw on a dark background.
    new CuplHighlighter(soListingView_->document(), CuplHighlighter::Language::SiListing,
                        CuplHighlighter::Theme::Dark);
    centralTabs_->addTab(soListingView_, QStringLiteral("Simulation Listing (.SO)"));

    // Tab 2: Simulation Source (.SI)
    auto *siTabContainer = new QWidget(centralTabs_);
    auto *siLay = new QVBoxLayout(siTabContainer);
    siLay->setContentsMargins(0, 0, 0, 0);
    siLay->setSpacing(0);

    auto *siBar = new QWidget(siTabContainer);
    auto *siBarLay = new QHBoxLayout(siBar);
    siBarLay->setContentsMargins(8, 4, 8, 4);
    // The caption is cut off rather than widening the window
    auto *siCaption = new QLabel(QStringLiteral("Direct .SI Test-Vector Source:"), siBar);
    siCaption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    siBarLay->addWidget(siCaption, 1);
    auto *applySiBtn = new QPushButton(QStringLiteral("Apply to Waveform"), siBar);
    applySiBtn->setToolTip(QStringLiteral("Apply the .SI source to the waveform"));
    auto *syncFromWaveBtn = new QPushButton(QStringLiteral("Refresh from Waveform"), siBar);
    syncFromWaveBtn->setToolTip(QStringLiteral("Rewrite the .SI source from the waveform"));
    siBarLay->addWidget(applySiBtn);
    siBarLay->addWidget(syncFromWaveBtn);
    siLay->addWidget(siBar);

    siSourceEdit_ = new QPlainTextEdit(siTabContainer);
    siSourceEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    siSourceEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    siSourceEdit_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background-color: #0f172a; color: #f1f5f9; }"));
    new CuplHighlighter(siSourceEdit_->document(), CuplHighlighter::Language::Si,
                        CuplHighlighter::Theme::Dark);
    siLay->addWidget(siSourceEdit_, 1);
    centralTabs_->addTab(siTabContainer, QStringLiteral("Simulation Source (.SI)"));

    connect(siSourceEdit_, &QPlainTextEdit::textChanged, this, [this]() {
        if (!syncingSiEditor_) {
            siEditorDirty_ = true;
        }
    });
    connect(applySiBtn, &QPushButton::clicked, this, &MainWindow::onApplySiSourceEdits);
    connect(syncFromWaveBtn, &QPushButton::clicked, this, [this]() {
        siEditorDirty_ = false;
        refreshCompanionTabs();
    });
    connect(centralTabs_, &QTabWidget::currentChanged, this, [this](int idx) {
        if (idx == 2 && !siEditorDirty_) {
            syncingSiEditor_ = true;
            siSourceEdit_->setPlainText(doc_.serializeSi());
            syncingSiEditor_ = false;
        }
    });
    connect(centralTabs_, &QTabWidget::currentChanged, this, &MainWindow::updateFindScope);

    // Bottom Dock: Simulator Console & Clickable Diagnostics
    simConsoleDock_ = new QDockWidget(QStringLiteral("Simulator Output & Diagnostics"), this);
    simConsoleDock_->setObjectName(QStringLiteral("SimConsoleDock"));
    auto *dockSplit = new QSplitter(Qt::Horizontal, simConsoleDock_);

    simConsoleOut_ = new QPlainTextEdit(dockSplit);
    simConsoleOut_->setReadOnly(true);
    simConsoleOut_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    simConsoleOut_->setPlaceholderText(QStringLiteral("Press F9 (Simulator -> Simulate) to run CSIM..."));

    diagList_ = new QListWidget(dockSplit);
    diagList_->setAlternatingRowColors(true);
    dockSplit->setStretchFactor(0, 3);
    dockSplit->setStretchFactor(1, 2);

    simConsoleDock_->setWidget(dockSplit);
    addDockWidget(Qt::BottomDockWidgetArea, simConsoleDock_);
    simConsoleDock_->setMaximumHeight(220);

    connect(diagList_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const int vecIdx = item->data(Qt::UserRole).toInt();
        const QString sig = item->data(Qt::UserRole + 1).toString();
        if (vecIdx >= 0) {
            centralTabs_->setCurrentIndex(0);
            waveformEditor_->jumpToSignalAndVector(sig, vecIdx);
        }
    });

    // Status bar
    cursorStatusLabel_ = new QLabel(QStringLiteral("Ready"), this);
    // The cursor line is long; it is cut off in a narrow window instead of
    // setting the window's minimum width.
    cursorStatusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    libStatusLabel_ = new QLabel(this);
    statusBar()->addWidget(cursorStatusLabel_, 1);
    statusBar()->addPermanentWidget(libStatusLabel_);
    refreshLibraryLabel();

    // Connect WaveformEditor signals
    connect(waveformEditor_, &WaveformEditorWidget::aboutToModifyDocument,
            this, &MainWindow::pushUndoSnapshot);
    connect(waveformEditor_, &WaveformEditorWidget::documentModified, this, [this]() {
        if (!siEditorDirty_) {
            syncingSiEditor_ = true;
            siSourceEdit_->setPlainText(doc_.serializeSi());
            syncingSiEditor_ = false;
        }
        refreshWindowTitle();
    });
    connect(waveformEditor_, &WaveformEditorWidget::cursorChanged,
            cursorStatusLabel_, &QLabel::setText);
    connect(waveformEditor_, &WaveformEditorWidget::signalSelectionChanged,
            this, &MainWindow::refreshSignalDefinitionPane);

    connect(waveformEditor_, &WaveformEditorWidget::requestAddSignal,
            this, &MainWindow::onAddSignal);
    connect(waveformEditor_, &WaveformEditorWidget::requestRenameSignal,
            this, &MainWindow::onRenameSignal);
    connect(waveformEditor_, &WaveformEditorWidget::requestDeleteSignal,
            this, &MainWindow::onDeleteSignal);
    connect(waveformEditor_, &WaveformEditorWidget::requestMoveSignal,
            this, &MainWindow::onMoveSignal);
    connect(waveformEditor_, &WaveformEditorWidget::requestSetBusValue,
            this, &MainWindow::onSetBusValue);
    connect(waveformEditor_, &WaveformEditorWidget::requestAddVectorDialog,
            this, &MainWindow::onAddVectorsDialog);
    connect(waveformEditor_, &WaveformEditorWidget::requestEditVectorAnnotation,
            this, &MainWindow::onEditVectorAnnotation);
}

void MainWindow::setupMenusAndToolbar() {
    auto deleteSelectedVectors = [this]() {
        if (!doc_.vectors.isEmpty()) {
            int v0, v1;
            waveformEditor_->selectedVectorRange(v0, v1);
            pushUndoSnapshot(QStringLiteral("Delete Vectors"));
            doc_.deleteVectors(v0, v1);
            waveformEditor_->refreshAll();
            refreshWindowTitle();
        }
    };

    // 1. File menu
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *actNew = fileMenu->addAction(lincuplIcon(QStringLiteral("new")), QStringLiteral("&New..."), QKeySequence::New, this, &MainWindow::onFileNew);
    fileMenu->addAction(QStringLiteral("New &Window"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this, [this]() {
        newWindow(this);
    });
    QAction *actOpen = fileMenu->addAction(lincuplIcon(QStringLiteral("open")), QStringLiteral("&Open..."), QKeySequence::Open, this, &MainWindow::onFileOpen);
    QAction *actSave = fileMenu->addAction(lincuplIcon(QStringLiteral("save")), QStringLiteral("&Save (.SI)"), QKeySequence::Save, this, &MainWindow::onFileSave);
    fileMenu->addAction(QStringLiteral("Save &As (.SI)..."), QKeySequence::SaveAs, this, &MainWindow::onFileSaveAs);
    fileMenu->addAction(QStringLiteral("Save A&ll"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S), this, []() {
        saveAll(true);
    });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Design &Properties..."), this, &MainWindow::onDesignProperties);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Export Waveform &PNG..."), this, &MainWindow::onExportPng);
    fileMenu->addAction(QStringLiteral("Export Waveform P&DF..."), this, &MainWindow::onExportPdf);
    QAction *actPrint = fileMenu->addAction(lincuplIcon(QStringLiteral("print")), QStringLiteral("&Print..."), QKeySequence::Print, this, &MainWindow::onPrint);
    // Print and PDF export in colour, or in black and white when unchecked.
    actPrintColors_ = fileMenu->addAction(QStringLiteral("Print &Colors"));
    actPrintColors_->setCheckable(true);
    actPrintColors_->setChecked(true);
    connect(actPrintColors_, &QAction::toggled, this, &MainWindow::settingsChanged);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Close Window"), QKeySequence::Close, this, &QWidget::close);
    // Exit closes every LinSim window, each asking to save its changes.
    QAction *actExit = fileMenu->addAction(QStringLiteral("E&xit"), QKeySequence::Quit, this, []() {
        QApplication::closeAllWindows();
    });
    actExit->setMenuRole(QAction::QuitRole);

    // 2. Edit menu
    QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    actUndo_ = editMenu->addAction(QStringLiteral("&Undo"), QKeySequence::Undo, this, &MainWindow::undo);
    actRedo_ = editMenu->addAction(QStringLiteral("&Redo"), QKeySequence::Redo, this, &MainWindow::redo);
    actUndo_->setEnabled(false);
    actRedo_->setEnabled(false);
    editMenu->addSeparator();
    QAction *actCut = editMenu->addAction(lincuplIcon(QStringLiteral("cut")), QStringLiteral("Cu&t Vectors"), QKeySequence::Cut, this, [this, deleteSelectedVectors]() {
        copySelectedVectors();
        deleteSelectedVectors();
    });
    QAction *actCopy = editMenu->addAction(lincuplIcon(QStringLiteral("copy")), QStringLiteral("&Copy Vectors"), QKeySequence::Copy, this, &MainWindow::copySelectedVectors);
    QAction *actPaste = editMenu->addAction(lincuplIcon(QStringLiteral("paste")), QStringLiteral("&Paste Vectors"), QKeySequence::Paste, this, &MainWindow::pasteVectors);
    editMenu->addAction(QStringLiteral("Select &All"), QKeySequence::SelectAll, waveformEditor_, &WaveformEditorWidget::selectAll);
    editMenu->addSeparator();
    QAction *actFind = editMenu->addAction(lincuplIcon(QStringLiteral("find")), QStringLiteral("&Find..."), QKeySequence::Find, this, &MainWindow::onFind);
    // The platform's Find Next / Find Previous keys, and F3 / Shift+F3 everywhere.
    auto withKey = [](QKeySequence::StandardKey standard, const QKeySequence &extra) {
        QList<QKeySequence> keys = QKeySequence::keyBindings(standard);
        if (!keys.contains(extra)) keys.append(extra);
        return keys;
    };
    actFindNext_ = editMenu->addAction(QStringLiteral("Find &Next"), this, [this]() { findNext(false); });
    actFindNext_->setShortcuts(withKey(QKeySequence::FindNext, QKeySequence(Qt::Key_F3)));
    actFindPrev_ = editMenu->addAction(QStringLiteral("Find Pre&vious"), this, [this]() { findNext(true); });
    actFindPrev_->setShortcuts(withKey(QKeySequence::FindPrevious, QKeySequence(Qt::SHIFT | Qt::Key_F3)));

    // 3. View menu (WinSim.cnt: Signal Definitions, Grid, Grid Size, Wide Signals, Colors + Zoom)
    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QAction *actZoomIn = viewMenu->addAction(lincuplIcon(QStringLiteral("zoom-in")), QStringLiteral("Zoom &In"), QKeySequence::ZoomIn, waveformEditor_, &WaveformEditorWidget::zoomIn);
    QAction *actZoomOut = viewMenu->addAction(lincuplIcon(QStringLiteral("zoom-out")), QStringLiteral("Zoom &Out"), QKeySequence::ZoomOut, waveformEditor_, &WaveformEditorWidget::zoomOut);
    QAction *actZoomFit = viewMenu->addAction(lincuplIcon(QStringLiteral("zoom-fit")), QStringLiteral("Zoom &Fit"), QKeySequence(Qt::CTRL | Qt::Key_0), waveformEditor_, &WaveformEditorWidget::zoomFit);
    QAction *actTaller = viewMenu->addAction(lincuplIcon(QStringLiteral("rows-taller")), QStringLiteral("&Taller Rows"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Equal), this, [this]() {
        waveformEditor_->setRowHeight(waveformEditor_->rowHeight() + 4);
        settingsChanged();
    });
    QAction *actShorter = viewMenu->addAction(lincuplIcon(QStringLiteral("rows-shorter")), QStringLiteral("S&horter Rows"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Minus), this, [this]() {
        waveformEditor_->setRowHeight(waveformEditor_->rowHeight() - 4);
        settingsChanged();
    });
    viewMenu->addSeparator();

    actShowGrid_ = viewMenu->addAction(QStringLiteral("&Grid"));
    actShowGrid_->setCheckable(true);
    actShowGrid_->setChecked(true);
    connect(actShowGrid_, &QAction::toggled, this, [this](bool on) {
        waveformEditor_->setShowGrid(on);
        settingsChanged();
    });

    viewMenu->addAction(QStringLiteral("Grid Si&ze..."), this, &MainWindow::onGridSizeDialog);

    actWideSignals_ = viewMenu->addAction(QStringLiteral("&Wide Signals"));
    actWideSignals_->setCheckable(true);
    actWideSignals_->setChecked(false);
    connect(actWideSignals_, &QAction::toggled, this, [this](bool on) {
        waveformEditor_->setWideSignals(on);
        settingsChanged();
    });

    actShowSigDef_ = viewMenu->addAction(QStringLiteral("&Signal Definitions"));
    actShowSigDef_->setCheckable(true);
    actShowSigDef_->setChecked(false);
    connect(actShowSigDef_, &QAction::toggled, this, [this](bool show) {
        sigDefView_->setVisible(show);
        if (show) refreshSignalDefinitionPane(waveformEditor_->selectedSignalIndex());
    });

    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("&Colors..."), this, &MainWindow::onColorsDialog);

    // 4. Signal menu (WinSim.cnt: Add/Delete/Rename Signal, Bus Members, Set Bus, Add/Insert/Delete Vector, Message)
    QMenu *sigMenu = menuBar()->addMenu(QStringLiteral("&Signal"));
    QAction *actAddSig = sigMenu->addAction(lincuplIcon(QStringLiteral("add-signal")), QStringLiteral("&Add Signal / Bus..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), this, &MainWindow::onAddSignal);
    sigMenu->addAction(QStringLiteral("&Rename Signal..."), this, [this]() {
        onRenameSignal(waveformEditor_->selectedSignalIndex());
    });
    QAction *actDelSig = sigMenu->addAction(lincuplIcon(QStringLiteral("delete-signal")), QStringLiteral("&Delete Signal"), this, [this]() {
        onDeleteSignal(waveformEditor_->selectedSignalIndex());
    });
    sigMenu->addAction(QStringLiteral("Move Signal &Up"), QKeySequence(Qt::ALT | Qt::Key_Up), this, [this]() {
        int idx = waveformEditor_->selectedSignalIndex();
        if (idx > 0) onMoveSignal(idx, idx - 1);
    });
    sigMenu->addAction(QStringLiteral("Move Signal Do&wn"), QKeySequence(Qt::ALT | Qt::Key_Down), this, [this]() {
        int idx = waveformEditor_->selectedSignalIndex();
        if (idx >= 0 && idx + 1 < doc_.signalsList.size()) onMoveSignal(idx, idx + 1);
    });

    sigMenu->addSeparator();
    sigMenu->addAction(QStringLiteral("Toggle &Bus Members"), this, [this]() {
        waveformEditor_->toggleBusExpanded(waveformEditor_->selectedSignalIndex());
    });
    sigMenu->addAction(QStringLiteral("Expand All Buses"), this, [this]() {
        waveformEditor_->setAllBusesExpanded(true);
    });
    sigMenu->addAction(QStringLiteral("Collapse All Buses"), this, [this]() {
        waveformEditor_->setAllBusesExpanded(false);
    });
    sigMenu->addAction(QStringLiteral("Set Bus &Value..."), QKeySequence(Qt::CTRL | Qt::Key_B), this, [this]() {
        int idx = waveformEditor_->selectedSignalIndex();
        if (idx >= 0 && idx < doc_.signalsList.size() && doc_.signalsList[idx].isBus()) {
            int v0, v1;
            waveformEditor_->selectedVectorRange(v0, v1);
            onSetBusValue(idx, v0, v1);
        }
    });

    sigMenu->addSeparator();
    QAction *actAddVec = sigMenu->addAction(lincuplIcon(QStringLiteral("add-vector")), QStringLiteral("Add &Vectors..."), QKeySequence(Qt::CTRL | Qt::Key_I), this, &MainWindow::onAddVectorsDialog);
    sigMenu->addAction(QStringLiteral("D&uplicate Current Vector"), QKeySequence(Qt::CTRL | Qt::Key_D), this, [this]() {
        if (!doc_.vectors.isEmpty()) {
            pushUndoSnapshot(QStringLiteral("Duplicate Vector"));
            doc_.duplicateVector(waveformEditor_->cursorVector());
            waveformEditor_->refreshAll();
            refreshWindowTitle();
        }
    });
    QAction *actDelVec = sigMenu->addAction(lincuplIcon(QStringLiteral("delete-vector")), QStringLiteral("De&lete Selected Vector(s)"), this, deleteSelectedVectors);
    sigMenu->addAction(QStringLiteral("Edit Vector &Message / Comment..."), this, [this]() {
        if (!doc_.vectors.isEmpty()) onEditVectorAnnotation(waveformEditor_->cursorVector());
    });

    // Quick state submenu under Signal menu as well
    sigMenu->addSeparator();
    QMenu *stateSub = sigMenu->addMenu(QStringLiteral("Set Selection State"));
    for (const auto &pair : {
             qMakePair(QLatin1Char('0'), "0 - Drive Input Low"),
             qMakePair(QLatin1Char('1'), "1 - Drive Input High"),
             qMakePair(QLatin1Char('L'), "L - Test Output Low"),
             qMakePair(QLatin1Char('H'), "H - Test Output High"),
             qMakePair(QLatin1Char('Z'), "Z - High Impedance"),
             qMakePair(QLatin1Char('X'), "X - Don't Care"),
             qMakePair(QLatin1Char('*'), "* - Simulator Determined"),
             qMakePair(QLatin1Char('P'), "P - Register Preload"),
             qMakePair(QLatin1Char('C'), "C - Clock (0-1-0)"),
             qMakePair(QLatin1Char('K'), "K - Clock (1-0-1)"),
         }) {
        stateSub->addAction(QString::fromLatin1(pair.second), this, [this, ch = pair.first]() {
            waveformEditor_->applyStateToSelection(ch);
        });
    }

    // 5. Simulator menu (WinSim.cnt: Simulate F9, Compile & Simulate, Assign Simulator Results, Set Library)
    QMenu *simMenu = menuBar()->addMenu(QStringLiteral("S&imulator"));
    QAction *actSimulate = simMenu->addAction(lincuplIcon(QStringLiteral("simulate")), QStringLiteral("&Simulate"), QKeySequence(Qt::Key_F9), this, [this]() {
        runSimulation(false, true);
    });
    QAction *actCompSim = simMenu->addAction(lincuplIcon(QStringLiteral("compile-simulate")), QStringLiteral("&Compile && Simulate"), QKeySequence(Qt::SHIFT | Qt::Key_F9), this, [this]() {
        runSimulation(true, true);
    });
    simMenu->addSeparator();
    simMenu->addAction(QStringLiteral("&Assign Simulator Results"), this, &MainWindow::onAssignSimulatorResults);
    simMenu->addSeparator();

    actAppendJed_ = simMenu->addAction(QStringLiteral("Append Vectors to &JEDEC (-j)"));
    actAppendJed_->setCheckable(true);
    actAppendJed_->setChecked(appendVectorsToJedec_);
    connect(actAppendJed_, &QAction::toggled, this, [this](bool on) {
        appendVectorsToJedec_ = on;
        settingsChanged();
    });

    actUsePldName_ = simMenu->addAction(QStringLiteral("Use PLD Filename for JEDEC (-&n)"));
    actUsePldName_->setCheckable(true);
    actUsePldName_->setChecked(usePldFilename_);
    connect(actUsePldName_, &QAction::toggled, this, [this](bool on) {
        usePldFilename_ = on;
        settingsChanged();
    });

    simMenu->addSeparator();
    simMenu->addAction(QStringLiteral("Set &Library..."), this, &MainWindow::onSetLibraryDialog);

    // 6. Window menu
    QMenu *winMenu = menuBar()->addMenu(QStringLiteral("&Window"));
    winMenu->addAction(QStringLiteral("&Waveform Editor"), QKeySequence(Qt::ALT | Qt::Key_1), this, [this]() {
        centralTabs_->setCurrentIndex(0);
    });
    winMenu->addAction(QStringLiteral("Simulation &Listing (.SO)"), QKeySequence(Qt::ALT | Qt::Key_2), this, [this]() {
        centralTabs_->setCurrentIndex(1);
    });
    winMenu->addAction(QStringLiteral("Simulation &Source (.SI)"), QKeySequence(Qt::ALT | Qt::Key_3), this, [this]() {
        centralTabs_->setCurrentIndex(2);
    });
    winMenu->addSeparator();
    winMenu->addAction(simConsoleDock_->toggleViewAction());
    winMenu->addSeparator();
    QAction *actCascade = winMenu->addAction(QStringLiteral("&Cascade"), this, [this]() {
        arrangeWindows(false, screen());
    });
    QAction *actTile = winMenu->addAction(QStringLiteral("&Tile"), this, [this]() {
        arrangeWindows(true, screen());
    });
    actCascade->setVisible(canArrangeWindows());
    actTile->setVisible(canArrangeWindows());
    // The open LinSim windows are listed between these separators; see
    // rebuildWindowList().
    winMenu->addSeparator();
    windowListEnd_ = winMenu->addSeparator();
    QAction *actRetCupl = winMenu->addAction(lincuplIcon(QStringLiteral("app-lincupl")), QStringLiteral("Open &LinCUPL IDE"), this, &MainWindow::onLaunchLinCupl);
    windowMenu_ = winMenu;
    connect(winMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildWindowList);

    // 7. Help menu
    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    Help::addHelpMenuActions(helpMenu, Help::Context::Simulator);
    helpMenu->addSeparator();
    helpMenu->addAction(QStringLiteral("&Waveform States && Shortcuts..."), this, &MainWindow::onShowQuickReference);
    helpMenu->addAction(QStringLiteral("&About LinSim..."), this, &MainWindow::onAbout);

    // Toolbar: the buttons of the original program in its order; the
    // horizontal grid size is the zoom, plus Zoom Fit, Cut/Copy/Paste and Find.
    QToolBar *tb = addToolBar(QStringLiteral("Main Toolbar"));
    tb->setObjectName(QStringLiteral("MainToolbar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(24, 24));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const QList<QAction *> toolbarActions = {
        actNew, actOpen, actSave, actPrint, nullptr,
        actCut, actCopy, actPaste, actFind, nullptr,
        actAddSig, actDelSig, actAddVec, actDelVec, nullptr,
        actSimulate, actCompSim, nullptr,
        actZoomIn, actZoomOut, actZoomFit, actTaller, actShorter, nullptr,
        actRetCupl
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

void MainWindow::loadSettings() {
    loadSharedSettings();

    loadingSettings_ = true;
    QSettings s;
    waveformEditor_->setCellWidth(s.value(QStringLiteral("View/cellWidth"), 36).toInt());
    bool wide = s.value(QStringLiteral("View/wideSignals"), false).toBool();
    actWideSignals_->setChecked(wide);
    waveformEditor_->setRowHeight(s.value(QStringLiteral("View/rowHeight"), wide ? 44 : 30).toInt());
    bool grid = s.value(QStringLiteral("View/showGrid"), true).toBool();
    actShowGrid_->setChecked(grid);
    loadingSettings_ = false;
}

void MainWindow::loadSharedSettings() {
    loadingSettings_ = true;
    QSettings s;
    WaveformColorTheme theme = waveformEditor_->colorTheme();
    theme.load(s);
    waveformEditor_->setColorTheme(theme);

    customLibraryPath_ = s.value(QStringLiteral("Simulator/libraryPath")).toString();
    appendVectorsToJedec_ = s.value(QStringLiteral("Simulator/appendJedec"), false).toBool();
    usePldFilename_ = s.value(QStringLiteral("Simulator/usePldName"), false).toBool();
    actAppendJed_->setChecked(appendVectorsToJedec_);
    actUsePldName_->setChecked(usePldFilename_);
    actPrintColors_->setChecked(s.value(QStringLiteral("Print/colors"), true).toBool());
    refreshLibraryLabel();
    loadingSettings_ = false;
}

void MainWindow::saveSettings() const {
    QSettings s;
    waveformEditor_->colorTheme().save(s);
    s.setValue(QStringLiteral("View/cellWidth"), waveformEditor_->cellWidth());
    s.setValue(QStringLiteral("View/rowHeight"), waveformEditor_->rowHeight());
    s.setValue(QStringLiteral("View/wideSignals"), waveformEditor_->wideSignals());
    s.setValue(QStringLiteral("View/showGrid"), waveformEditor_->showGrid());
    s.setValue(QStringLiteral("Simulator/libraryPath"), customLibraryPath_);
    s.setValue(QStringLiteral("Simulator/appendJedec"), appendVectorsToJedec_);
    s.setValue(QStringLiteral("Simulator/usePldName"), usePldFilename_);
    s.setValue(QStringLiteral("Print/colors"), actPrintColors_->isChecked());
}

void MainWindow::settingsChanged() {
    if (loadingSettings_) return;
    saveSettings();
    for (MainWindow *w : windows()) {
        if (w != this) w->loadSharedSettings();
    }
}

void MainWindow::rebuildWindowList() {
    qDeleteAll(windowListActions_);
    windowListActions_.clear();
    const QList<MainWindow *> wins = windows();
    for (int i = 0; i < wins.size(); ++i) {
        MainWindow *w = wins[i];
        QString name = w->documentDisplayName();
        if (w->doc_.modified || w->siEditorDirty_) name += QStringLiteral(" *");
        name.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto *a = new QAction(i < 9 ? QStringLiteral("&%1 %2").arg(i + 1).arg(name) : name, windowMenu_);
        a->setCheckable(true);
        a->setChecked(w == this);
        connect(a, &QAction::triggered, this, [win = QPointer<MainWindow>(w)]() {
            if (!win) return;
            if (win->isMinimized()) win->showNormal();
            win->show();
            win->raise();
            win->activateWindow();
        });
        windowMenu_->insertAction(windowListEnd_, a);
        windowListActions_.append(a);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (simulationRunning_) {
        // The simulation runs in a nested event loop of this window.
        statusBar()->showMessage(QStringLiteral("Wait for the simulation to finish before closing."), 4000);
        event->ignore();
        return;
    }
    if (!maybeSaveInteractive()) {
        event->ignore();
        return;
    }
    event->accept();
}

bool MainWindow::maybeSaveInteractive() {
    if (!doc_.modified && !siEditorDirty_) return true;
    const auto ans = QMessageBox::question(
        this, QStringLiteral("Save Changes"),
        QStringLiteral("Save changes to simulation '%1' before closing?")
            .arg(doc_.filePath.isEmpty() ? doc_.header.name : QFileInfo(doc_.filePath).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (ans == QMessageBox::Save) return onFileSave();
    if (ans == QMessageBox::Cancel) return false;
    return true;
}

void MainWindow::pushUndoSnapshot(const QString &description) {
    Q_UNUSED(description);
    undoStack_.append(doc_);
    if (undoStack_.size() > 64) {
        undoStack_.removeFirst();
    }
    redoStack_.clear();
    actUndo_->setEnabled(true);
    actRedo_->setEnabled(false);
}

void MainWindow::undo() {
    if (undoStack_.isEmpty()) return;
    redoStack_.append(doc_);
    doc_ = undoStack_.takeLast();
    waveformEditor_->rebuildRows();
    refreshCompanionTabs();
    refreshWindowTitle();
    actUndo_->setEnabled(!undoStack_.isEmpty());
    actRedo_->setEnabled(true);
}

void MainWindow::redo() {
    if (redoStack_.isEmpty()) return;
    undoStack_.append(doc_);
    doc_ = redoStack_.takeLast();
    waveformEditor_->rebuildRows();
    refreshCompanionTabs();
    refreshWindowTitle();
    actUndo_->setEnabled(true);
    actRedo_->setEnabled(!redoStack_.isEmpty());
}

void MainWindow::copySelectedVectors() {
    if (doc_.vectors.isEmpty()) return;
    int v0, v1;
    waveformEditor_->selectedVectorRange(v0, v1);
    vectorClipboard_.clear();
    QStringList clipLines;
    for (int v = v0; v <= v1; ++v) {
        vectorClipboard_.append(doc_.vectors[v]);
        QString rowStr;
        for (const QString &pin : doc_.allOrderedPins()) {
            rowStr.append(doc_.pinState(v, pin));
        }
        clipLines.append(rowStr);
    }
    QApplication::clipboard()->setText(clipLines.join(QLatin1Char('\n')));
    statusBar()->showMessage(QStringLiteral("Copied %1 vector(s)").arg(vectorClipboard_.size()), 3000);
}

void MainWindow::pasteVectors() {
    if (vectorClipboard_.isEmpty()) return;
    pushUndoSnapshot(QStringLiteral("Paste Vectors"));
    int insertAt = doc_.vectors.isEmpty() ? 0 : (waveformEditor_->cursorVector() + 1);
    for (int i = 0; i < vectorClipboard_.size(); ++i) {
        doc_.vectors.insert(insertAt + i, vectorClipboard_[i]);
    }
    doc_.modified = true;
    waveformEditor_->refreshAll();
    refreshCompanionTabs();
    refreshWindowTitle();
}

namespace {

// Finds the next match in a text tab, wrapping around once.
bool findInTextEdit(QPlainTextEdit *ed, const QString &needle, bool caseSensitive,
                    bool wholeWords, bool backward) {
    if (needle.isEmpty()) return false;
    QTextDocument::FindFlags flags;
    if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;
    if (wholeWords) flags |= QTextDocument::FindWholeWords;
    if (backward) flags |= QTextDocument::FindBackward;
    if (ed->find(needle, flags)) return true;

    const QTextCursor saved = ed->textCursor();
    QTextCursor wrap(ed->document());
    wrap.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
    ed->setTextCursor(wrap);
    if (ed->find(needle, flags)) return true;
    ed->setTextCursor(saved);
    return false;
}

} // namespace

bool MainWindow::find(const QString &needle, FindDialog::Target target, bool caseSensitive,
                      bool wholeWords, bool backward) {
    bool found = false;
    switch (centralTabs_->currentIndex()) {
    case 1:
        found = findInTextEdit(soListingView_, needle, caseSensitive, wholeWords, backward);
        break;
    case 2:
        found = findInTextEdit(siSourceEdit_, needle, caseSensitive, wholeWords, backward);
        break;
    default:
        switch (target) {
        case FindDialog::Target::SignalName:
            found = waveformEditor_->findSignal(needle, caseSensitive, wholeWords, backward);
            break;
        case FindDialog::Target::VectorNumber: {
            QString number = needle.trimmed();
            if (number.startsWith(QLatin1Char('#'))) number.remove(0, 1);
            bool ok = false;
            const int vec = number.toInt(&ok);
            found = ok && waveformEditor_->selectVector(vec - 1);
            break;
        }
        case FindDialog::Target::VectorText:
            found = waveformEditor_->findVectorText(needle, caseSensitive, wholeWords, backward);
            break;
        }
        break;
    }
    if (!found) {
        statusBar()->showMessage(QStringLiteral("'%1' not found").arg(needle), 4000);
    }
    return found;
}

FindDialog *MainWindow::findDialog() {
    if (!findDialog_) {
        findDialog_ = new FindDialog(this);
        // F3 and the other Find Next keys keep working while the box has focus.
        findDialog_->addActions({actFindNext_, actFindPrev_});
        connect(findDialog_, &FindDialog::findRequested, this, &MainWindow::findNext);
        updateFindScope();
    }
    return findDialog_;
}

void MainWindow::updateFindScope() {
    if (!findDialog_) return;
    const int idx = centralTabs_->currentIndex();
    findDialog_->setScope(centralTabs_->tabText(idx), idx == 0);
}

void MainWindow::onFind() {
    QString seed;
    if (centralTabs_->currentIndex() == 1) {
        seed = soListingView_->textCursor().selectedText();
    } else if (centralTabs_->currentIndex() == 2) {
        seed = siSourceEdit_->textCursor().selectedText();
    }
    if (seed.contains(QChar::ParagraphSeparator)) seed.clear();
    findDialog()->focusFind(seed);
}

bool MainWindow::findNext(bool backward) {
    if (!findDialog_ || findDialog_->text().isEmpty()) {
        onFind();
        return false;
    }
    const bool found = find(findDialog_->text(), findDialog_->target(),
                            findDialog_->caseSensitive(), findDialog_->wholeWords(), backward);
    findDialog_->setStatus(found ? QString() : QStringLiteral("No match found."));
    return found;
}

bool MainWindow::openDocument(const QString &path, QString *errorOut) {
    SimDocument loaded;
    if (!loaded.loadFile(path, errorOut)) {
        return false;
    }
    adoptDocument(std::move(loaded));
    return true;
}

void MainWindow::adoptDocument(SimDocument &&doc) {
    undoStack_.clear();
    redoStack_.clear();
    actUndo_->setEnabled(false);
    actRedo_->setEnabled(false);

    doc_ = std::move(doc);
    siEditorDirty_ = false;
    waveformEditor_->setDocument(&doc_);
    refreshCompanionTabs();
    refreshSignalDefinitionPane(waveformEditor_->selectedSignalIndex());
    refreshWindowTitle();
}

bool MainWindow::saveDocument(QString *errorOut) {
    if (siEditorDirty_) {
        onApplySiSourceEdits();
    }
    if (doc_.filePath.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("No file path set for document.");
        return false;
    }
    QString targetPath = doc_.filePath;
    if (targetPath.endsWith(QStringLiteral(".wo"), Qt::CaseInsensitive)) {
        const QString existingSi = SimDocument::findCompanionFile(targetPath, QStringLiteral(".si"));
        if (!existingSi.isEmpty()) {
            targetPath = existingSi;
        } else {
            QFileInfo fi(targetPath);
            targetPath = fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".si"));
        }
    }
    if (!doc_.saveSiFile(targetPath, errorOut)) {
        return false;
    }
    refreshCompanionTabs();
    refreshWindowTitle();
    return true;
}

bool MainWindow::saveDocumentAs(const QString &path, QString *errorOut) {
    if (siEditorDirty_) {
        onApplySiSourceEdits();
    }
    if (!doc_.saveSiFile(path, errorOut)) {
        return false;
    }
    refreshCompanionTabs();
    refreshWindowTitle();
    return true;
}

QString MainWindow::soListingText() const {
    return soListingView_->toPlainText();
}

QString MainWindow::siSourceText() const {
    return siSourceEdit_->toPlainText();
}

void MainWindow::refreshWindowTitle() {
    QString docName = documentDisplayName();
    if (doc_.modified || siEditorDirty_) {
        docName += QStringLiteral(" *");
    }
    setWindowTitle(QStringLiteral("%1 — LinSim [%2]").arg(
        docName, doc_.header.device.isEmpty() ? QStringLiteral("PLD") : doc_.header.device));
}

void MainWindow::refreshCompanionTabs() {
    // Update .SI source tab
    syncingSiEditor_ = true;
    siSourceEdit_->setPlainText(doc_.serializeSi());
    syncingSiEditor_ = false;
    siEditorDirty_ = false;

    // Update .SO listing tab if companion .so exists
    if (!doc_.filePath.isEmpty()) {
        const QString soPath = SimDocument::findCompanionFile(doc_.filePath, QStringLiteral(".so"));
        if (!soPath.isEmpty()) {
            QFile sf(soPath);
            if (sf.open(QIODevice::ReadOnly)) {
                soListingView_->setPlainText(QString::fromLatin1(sf.readAll()));
                return;
            }
        }
    }
    if (soListingView_->toPlainText().isEmpty()) {
        soListingView_->setPlainText(
            QStringLiteral("No .SO simulation listing file found yet.\nRun Simulator -> Simulate (F9) to generate listing."));
    }
}

void MainWindow::refreshSignalDefinitionPane(int sigIndex) {
    if (sigIndex < 0 || sigIndex >= doc_.signalsList.size()) {
        sigDefView_->setPlainText(QStringLiteral("Select a signal to inspect its pin assignment and compiled equations."));
        return;
    }

    const SignalItem &sig = doc_.signalsList[sigIndex];
    QStringList lines;
    lines.append(QStringLiteral("Signal: %1   |   Type: %2   |   Width: %3 bit(s)")
                     .arg(sig.displayLabel(),
                          sig.isBus() ? QStringLiteral("Bus") : (doc_.isSignalOutput(sig) ? QStringLiteral("Output") : QStringLiteral("Input")))
                     .arg(sig.width()));

    if (sig.isBus()) {
        lines.append(QStringLiteral("Members (MSB..LSB): [%1]").arg(sig.members.join(QStringLiteral(", "))));
    }

    for (const QString &m : sig.members) {
        auto it = doc_.pinMeta.find(m.toLower());
        if (it != doc_.pinMeta.end()) {
            QString headerLine = QStringLiteral("• %1").arg(m);
            if (!it->pinNumber.isEmpty()) headerLine += QStringLiteral(" (Pin %1)").arg(it->pinNumber);
            if (it->activeLowInDesign) headerLine += QStringLiteral(" [Active-Low in PLD]");
            lines.append(headerLine);
            if (!it->equation.isEmpty()) {
                for (const QString &eqL : it->equation.split(QLatin1Char('\n'))) {
                    lines.append(QStringLiteral("    ") + eqL);
                }
            }
        }
    }

    sigDefView_->setPlainText(lines.join(QLatin1Char('\n')));
}

void MainWindow::populateDiagnosticsFromLog(const QString &logText) {
    diagList_->clear();

    // Parse CSIM vector number context (e.g. "0002: C000  LLLH L") and "[0019sa] ..." lines
    const QStringList lines = logText.split(QRegularExpression(QStringLiteral("\r?\n")));
    int currentVecIdx = -1;
    static const QRegularExpression reVecNum(QStringLiteral("^(\\d{4}):\\s+"));
    static const QRegularExpression reDiag(QStringLiteral("^\\[(\\d{4}[A-Za-z]{2})\\]\\s*(.+)$"));
    static const QRegularExpression reMismatchSig(QStringLiteral("for\\s+([A-Za-z0-9_\\.]+)\\s*$"));

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        auto mv = reVecNum.match(trimmed);
        if (mv.hasMatch()) {
            currentVecIdx = mv.captured(1).toInt() - 1;
            continue;
        }
        auto md = reDiag.match(trimmed);
        if (md.hasMatch()) {
            const QString code = md.captured(1);
            const QString msg = md.captured(2);
            QString sigName;
            auto ms = reMismatchSig.match(msg);
            if (ms.hasMatch()) sigName = ms.captured(1);

            const QString label = (currentVecIdx >= 0)
                                      ? QStringLiteral("[Vec %1] [%2] %3")
                                            .arg(currentVecIdx + 1, 4, 10, QLatin1Char('0'))
                                            .arg(code, msg)
                                      : QStringLiteral("[%1] %2").arg(code, msg);
            auto *item = new QListWidgetItem(label, diagList_);
            item->setForeground(QColor(248, 113, 113));
            item->setData(Qt::UserRole, currentVecIdx);
            item->setData(Qt::UserRole + 1, sigName);
        }
    }

    if (diagList_->count() == 0) {
        auto *okItem = new QListWidgetItem(
            QStringLiteral("✓ Simulation completed cleanly (%1 vectors, 0 errors)").arg(doc_.vectors.size()),
            diagList_);
        okItem->setForeground(QColor(74, 222, 128));
        okItem->setData(Qt::UserRole, -1);
    }
}

bool MainWindow::runSimulation(bool compileFirst, bool interactive, QString *logOut) {
    if (simulationRunning_) {
        if (logOut) *logOut = QStringLiteral("A simulation is already running in this window.");
        return false;
    }
    simulationRunning_ = true;
    const auto clearRunning = qScopeGuard([this]() { simulationRunning_ = false; });

    if (siEditorDirty_) {
        onApplySiSourceEdits();
    }

    if (doc_.filePath.isEmpty()) {
        if (!interactive || !onFileSaveAs()) {
            if (logOut) *logOut = QStringLiteral("Cannot simulate unsaved document.");
            return false;
        }
    }

    // Save the .si for csim only when it differs from the file on disk, so
    // an unchanged file keeps its own formatting
    QString saveErr;
    const bool needSave = doc_.modified || doc_.loadedFromWo || !QFileInfo::exists(doc_.filePath);
    if (needSave && !saveDocument(&saveErr)) {
        if (logOut) *logOut = saveErr;
        if (interactive) QMessageBox::critical(this, QStringLiteral("Save Error"), saveErr);
        return false;
    }

    const QFileInfo siInfo(doc_.filePath);
    const QString workDir = siInfo.absolutePath();
    const QString stem = siInfo.completeBaseName();
    const QString libPath = resolveActiveLibrary();
    libStatusLabel_->setText(libPath.isEmpty()
                                 ? QStringLiteral("Lib: (default)")
                                 : QStringLiteral("Lib: %1").arg(QFileInfo(libPath).fileName()));

    QString combinedLog;
    simConsoleOut_->clear();
    diagList_->clear();
    simConsoleDock_->show();

    auto runTool = [&](const QString &exe, const QStringList &args, int &exitCodeOut) -> bool {
        combinedLog += QStringLiteral("$ %1 %2\n").arg(exe, args.join(QLatin1Char(' ')));
        simConsoleOut_->appendPlainText(QStringLiteral("$ %1 %2").arg(exe, args.join(QLatin1Char(' '))));

        QProcess proc;
        proc.setWorkingDirectory(workDir);
        proc.setProcessChannelMode(QProcess::MergedChannels);

        if (!libPath.isEmpty()) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("LIBCUPL"), libPath);
            proc.setProcessEnvironment(env);
        }

        proc.start(exe, args);
        if (!proc.waitForStarted(5000)) {
            const QString err = QStringLiteral("Failed to launch '%1': %2").arg(exe, proc.errorString());
            combinedLog += err + QLatin1Char('\n');
            simConsoleOut_->appendPlainText(err);
            exitCodeOut = -1;
            return false;
        }

        if (interactive) {
            QEventLoop loop;
            connect(&proc, &QProcess::readyReadStandardOutput, &loop, [&]() {
                const QString chunk = QString::fromLatin1(proc.readAllStandardOutput());
                combinedLog += chunk;
                simConsoleOut_->moveCursor(QTextCursor::End);
                simConsoleOut_->insertPlainText(chunk);
            });
            connect(&proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    &loop, &QEventLoop::quit);
            loop.exec();
        } else {
            proc.waitForFinished(30000);
        }

        const QString rem = QString::fromLatin1(proc.readAllStandardOutput());
        if (!rem.isEmpty()) {
            combinedLog += rem;
            simConsoleOut_->moveCursor(QTextCursor::End);
            simConsoleOut_->insertPlainText(rem);
        }
        exitCodeOut = (proc.exitStatus() == QProcess::NormalExit) ? proc.exitCode() : -1;
        return proc.exitStatus() == QProcess::NormalExit;
    };

    // Check if companion .abs exists; if missing (or compileFirst requested) and .pld exists, compile first!
    const QString existingAbs = SimDocument::findCompanionFile(doc_.filePath, QStringLiteral(".abs"));
    const QString companionPld = SimDocument::findCompanionFile(doc_.filePath, QStringLiteral(".pld"));
    if ((compileFirst || existingAbs.isEmpty()) && !companionPld.isEmpty()) {
        const QString cuplBin = findCliBinary(QStringLiteral("cupl"));
        if (cuplBin.isEmpty()) {
            const QString msg = QStringLiteral("Could not find 'cupl' compiler binary to build .abs file.");
            if (logOut) *logOut = msg;
            if (interactive) QMessageBox::warning(this, QStringLiteral("CUPL Not Found"), msg);
            return false;
        }
        QStringList cuplArgs = {QStringLiteral("-ja"), QStringLiteral("-m1")};
        if (usePldFilename_ || QFileInfo(companionPld).completeBaseName().compare(doc_.header.name, Qt::CaseInsensitive) != 0) {
            cuplArgs.append(QStringLiteral("-n"));
        }
        if (!libPath.isEmpty()) {
            cuplArgs << QStringLiteral("-u") << libPath;
        }
        cuplArgs.append(QFileInfo(companionPld).fileName());

        int cuplExit = 0;
        if (!runTool(cuplBin, cuplArgs, cuplExit) || cuplExit != 0) {
            populateDiagnosticsFromLog(combinedLog);
            if (logOut) *logOut = combinedLog;
            return false;
        }
    }

    const QString csimBin = findCliBinary(QStringLiteral("csim"));
    if (csimBin.isEmpty()) {
        const QString msg = QStringLiteral("Could not find sibling 'csim' simulator binary.");
        simConsoleOut_->appendPlainText(msg);
        if (logOut) *logOut = msg;
        if (interactive) QMessageBox::critical(this, QStringLiteral("CSIM Not Found"), msg);
        return false;
    }

    QString flagStr = QStringLiteral("-lw");
    if (appendVectorsToJedec_) flagStr += QLatin1Char('j');
    if (usePldFilename_) flagStr += QLatin1Char('n');

    QStringList csimArgs = {flagStr};
    if (!libPath.isEmpty()) {
        csimArgs << QStringLiteral("-u") << libPath;
    }
    csimArgs << stem;

    int csimExit = 0;
    const bool ranOk = runTool(csimBin, csimArgs, csimExit);

    // Reload generated .wo waveform results (even if csimExit == 1 due to #E expected-vs-actual mismatches!)
    const QString woPath = SimDocument::findCompanionFile(doc_.filePath, QStringLiteral(".wo"));
    if (!woPath.isEmpty()) {
        QString mergeErr;
        if (doc_.mergeSimulatedWo(woPath, &mergeErr)) {
            doc_.loadCompanionMetadata(doc_.filePath);
            waveformEditor_->rebuildRows();
            refreshSignalDefinitionPane(waveformEditor_->selectedSignalIndex());
        }
    }

    refreshCompanionTabs();
    populateDiagnosticsFromLog(combinedLog);
    refreshWindowTitle();

    if (logOut) *logOut = combinedLog;
    return ranOk && (csimExit == 0);
}

void MainWindow::onFileNew() {
    SimDocument fresh = SimDocument::createDefaultNew();
    DesignPropertiesDialog dlg(fresh.header, fresh.fileBase, this);
    if (dlg.exec() != QDialog::Accepted) return;

    fresh.header = dlg.headerResult();
    fresh.fileBase = dlg.baseResult();
    fresh.hasExplicitBase = (fresh.fileBase != BusRadix::Hex);
    fresh.modified = false;

    // A window that already holds a document keeps it; the new one opens in
    // a window of its own.
    MainWindow *target = isUntouched() ? this : newWindow(this);
    target->adoptDocument(std::move(fresh));
}

void MainWindow::onFileOpen() {
    const QString startDir = doc_.filePath.isEmpty() ? QString() : QFileInfo(doc_.filePath).absolutePath();
    const QString fn = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Simulation or Waveform File"), startDir,
        QStringLiteral("Simulation & Waveform Files (*.si *.SI *.wo *.WO);;Simulation Input (*.si *.SI);;Waveform Output (*.wo *.WO);;All Files (*)"));
    if (fn.isEmpty()) return;

    QString err;
    if (!openInWindow(fn, this, &err)) {
        QMessageBox::critical(this, QStringLiteral("Error Opening File"), err);
    }
}

bool MainWindow::onFileSave() {
    if (doc_.filePath.isEmpty() || doc_.loadedFromWo) {
        return onFileSaveAs();
    }
    QString err;
    if (!saveDocument(&err)) {
        QMessageBox::critical(this, QStringLiteral("Save Error"), err);
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(doc_.filePath), 3000);
    return true;
}

bool MainWindow::onFileSaveAs() {
    QString suggested = doc_.filePath;
    if (suggested.isEmpty()) {
        suggested = (doc_.header.name.isEmpty() ? QStringLiteral("design") : doc_.header.name) + QStringLiteral(".si");
    } else if (suggested.endsWith(QStringLiteral(".wo"), Qt::CaseInsensitive)) {
        QFileInfo fi(suggested);
        suggested = fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".si"));
    }

    const QString fn = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save Simulation Input (.SI)"), suggested,
        QStringLiteral("CUPL Simulation Files (*.si *.SI);;All Files (*)"));
    if (fn.isEmpty()) return false;

    QString err;
    if (!saveDocumentAs(fn, &err)) {
        QMessageBox::critical(this, QStringLiteral("Save Error"), err);
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(doc_.filePath), 3000);
    return true;
}

void MainWindow::onDesignProperties() {
    DesignPropertiesDialog dlg(doc_.header, doc_.fileBase, this);
    if (dlg.exec() == QDialog::Accepted) {
        pushUndoSnapshot(QStringLiteral("Edit Design Properties"));
        doc_.header = dlg.headerResult();
        doc_.fileBase = dlg.baseResult();
        doc_.hasExplicitBase = (doc_.fileBase != BusRadix::Hex);
        doc_.modified = true;
        refreshCompanionTabs();
        refreshWindowTitle();
    }
}

void MainWindow::onExportPng() {
    const QString fn = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Waveform Image (PNG)"),
        (doc_.header.name.isEmpty() ? QStringLiteral("waveform") : doc_.header.name) + QStringLiteral(".png"),
        QStringLiteral("PNG Images (*.png);;All Files (*)"));
    if (fn.isEmpty()) return;

    QString err;
    if (!waveformEditor_->exportPng(fn, &err)) {
        QMessageBox::critical(this, QStringLiteral("Export Error"), err);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Exported PNG to %1").arg(fn), 4000);
}

void MainWindow::onExportPdf() {
    const QString fn = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Waveform Document (PDF)"),
        (doc_.header.name.isEmpty() ? QStringLiteral("waveform") : doc_.header.name) + QStringLiteral(".pdf"),
        QStringLiteral("PDF Documents (*.pdf);;All Files (*)"));
    if (fn.isEmpty()) return;

    QString err;
    if (!waveformEditor_->exportPdf(fn, &err, printColors())) {
        QMessageBox::critical(this, QStringLiteral("Export Error"), err);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Exported PDF to %1").arg(fn), 4000);
}

bool MainWindow::printColors() const {
    return actPrintColors_->isChecked();
}

void MainWindow::setPrintColors(bool on) {
    actPrintColors_->setChecked(on);
}

void MainWindow::onPrint() {
#ifdef LINSIM_HAS_PRINTSUPPORT
    QPrinter printer(QPrinter::HighResolution);
    printer.setPageOrientation(QPageLayout::Landscape);
    QPrintDialog dlg(&printer, this);
    if (dlg.exec() != QDialog::Accepted) return;

    QPainter painter(&printer);
    if (!painter.isActive()) return;
    const QImage img = waveformEditor_->renderToImage(256, !printColors());
    const QRect area = painter.viewport();
    const QSize scaled = img.size().scaled(area.size(), Qt::KeepAspectRatio);
    painter.drawImage(QRect(area.topLeft(), scaled), img);
#else
    QMessageBox::information(this, QStringLiteral("Print"),
                             QStringLiteral("Printing requires Qt6::PrintSupport."));
#endif
}

void MainWindow::onAddSignal() {
    AddSignalDialog dlg(doc_, this);
    if (dlg.exec() != QDialog::Accepted) return;

    SignalItem item;
    bool beforeSelected = false;
    QString err;
    if (!dlg.buildSignalItem(item, beforeSelected, &err)) {
        QMessageBox::warning(this, QStringLiteral("Add Signal"), err);
        return;
    }

    pushUndoSnapshot(QStringLiteral("Add Signal"));
    const int insertPos = beforeSelected ? waveformEditor_->selectedSignalIndex() : -1;
    if (!doc_.addSignal(item, insertPos, QLatin1Char('\0'), &err)) {
        undoStack_.removeLast();
        QMessageBox::warning(this, QStringLiteral("Add Signal"), err);
        return;
    }

    waveformEditor_->rebuildRows();
    refreshCompanionTabs();
    refreshWindowTitle();
}

void MainWindow::onRenameSignal(int sigIndex) {
    if (sigIndex < 0 || sigIndex >= doc_.signalsList.size()) return;
    const SignalItem &sig = doc_.signalsList[sigIndex];

    bool ok = false;
    const QString input = QInputDialog::getText(
        this, QStringLiteral("Rename Signal"),
        QStringLiteral("Enter new name for signal '%1' (prefix with '!' for active-low):").arg(sig.displayLabel()),
        QLineEdit::Normal, sig.displayLabel(), &ok);
    if (!ok || input.trimmed().isEmpty()) return;

    pushUndoSnapshot(QStringLiteral("Rename Signal"));
    QString err;
    if (!doc_.renameSignal(sigIndex, input, sig.activeLow, &err)) {
        undoStack_.removeLast();
        QMessageBox::warning(this, QStringLiteral("Rename Signal"), err);
        return;
    }

    waveformEditor_->rebuildRows();
    refreshCompanionTabs();
    refreshWindowTitle();
}

void MainWindow::onDeleteSignal(int sigIndex) {
    if (sigIndex < 0 || sigIndex >= doc_.signalsList.size()) return;
    if (doc_.signalsList.size() <= 1) {
        QMessageBox::information(this, QStringLiteral("Delete Signal"),
                                 QStringLiteral("At least one signal must remain in ORDER:."));
        return;
    }
    pushUndoSnapshot(QStringLiteral("Delete Signal"));
    doc_.deleteSignal(sigIndex);
    waveformEditor_->rebuildRows();
    refreshCompanionTabs();
    refreshWindowTitle();
}

void MainWindow::onMoveSignal(int fromIdx, int toIdx) {
    if (fromIdx < 0 || toIdx < 0 || fromIdx >= doc_.signalsList.size() || toIdx >= doc_.signalsList.size()) return;
    pushUndoSnapshot(QStringLiteral("Reorder Signal"));
    doc_.moveSignal(fromIdx, toIdx);
    waveformEditor_->rebuildRows();
    waveformEditor_->jumpToSignalAndVector(doc_.signalsList[toIdx].name, waveformEditor_->cursorVector());
    refreshCompanionTabs();
    refreshWindowTitle();
}

void MainWindow::onSetBusValue(int sigIndex, int startVec, int endVec) {
    if (sigIndex < 0 || sigIndex >= doc_.signalsList.size()) return;
    if (doc_.vectors.isEmpty()) return;

    SetBusValueDialog dlg(doc_, sigIndex, startVec, endVec, this);
    if (dlg.exec() != QDialog::Accepted) return;

    pushUndoSnapshot(QStringLiteral("Set Bus Value"));
    QString err;
    if (!doc_.setBusValueFromString(sigIndex, dlg.startVector(), dlg.endVector(),
                                    dlg.valueText(), dlg.radix(), dlg.driveAsOutput(), &err)) {
        undoStack_.removeLast();
        QMessageBox::warning(this, QStringLiteral("Invalid Bus Value"), err);
        return;
    }
    doc_.signalsList[sigIndex].displayRadix = dlg.radix();
    waveformEditor_->refreshAll();
    refreshCompanionTabs();
    refreshWindowTitle();
}

void MainWindow::onAddVectorsDialog() {
    AddVectorDialog dlg(waveformEditor_->cursorVector(), doc_.vectors.size(), this);
    if (dlg.exec() != QDialog::Accepted) return;

    pushUndoSnapshot(QStringLiteral("Add Vectors"));
    int pos = doc_.vectors.size();
    if (dlg.positionResult() == AddVectorDialog::Position::InsertBeforeCursor) {
        pos = waveformEditor_->cursorVector();
    } else if (dlg.positionResult() == AddVectorDialog::Position::InsertAfterCursor) {
        pos = qMin(doc_.vectors.size(), waveformEditor_->cursorVector() + 1);
    }

    const auto mode = dlg.initModeResult();
    if (mode == AddVectorDialog::InitMode::CopyNeighbor) {
        doc_.insertVector(pos, dlg.countResult(), QLatin1Char('0'), QLatin1Char('*'), true);
    } else if (mode == AddVectorDialog::InitMode::AllDontCare) {
        doc_.insertVector(pos, dlg.countResult(), QLatin1Char('X'), QLatin1Char('X'), false);
    } else {
        doc_.insertVector(pos, dlg.countResult(), QLatin1Char('0'), QLatin1Char('*'), false);
    }

    waveformEditor_->refreshAll();
    refreshCompanionTabs();
    refreshWindowTitle();
}

void MainWindow::onEditVectorAnnotation(int vecIndex) {
    if (vecIndex < 0 || vecIndex >= doc_.vectors.size()) return;
    VectorRow &vr = doc_.vectors[vecIndex];
    VectorAnnotationDialog dlg(vecIndex, vr.comment, vr.messagesBefore, this);
    if (dlg.exec() != QDialog::Accepted) return;

    pushUndoSnapshot(QStringLiteral("Edit Vector Annotation"));
    vr.comment = dlg.commentResult();
    vr.messagesBefore = dlg.messagesResult();
    doc_.modified = true;
    waveformEditor_->refreshAll();
    refreshCompanionTabs();
    refreshWindowTitle();
}

void MainWindow::onGridSizeDialog() {
    GridSizeDialog dlg(waveformEditor_->cellWidth(), waveformEditor_->rowHeight(),
                       waveformEditor_->showGrid(), waveformEditor_->wideSignals(), this);
    if (dlg.exec() != QDialog::Accepted) return;

    actWideSignals_->setChecked(dlg.wideSignals());
    actShowGrid_->setChecked(dlg.showGrid());
    waveformEditor_->setCellWidth(dlg.cellWidth());
    waveformEditor_->setRowHeight(dlg.rowHeight());
    settingsChanged();
}

void MainWindow::onColorsDialog() {
    WaveformColorsDialog dlg(waveformEditor_->colorTheme(), this);
    if (dlg.exec() == QDialog::Accepted) {
        waveformEditor_->setColorTheme(dlg.themeResult());
        settingsChanged();
    }
}

void MainWindow::onSetLibraryDialog() {
    SetLibraryDialog dlg(customLibraryPath_, discoverLibraryCandidates(),
                         appendVectorsToJedec_, usePldFilename_, this);
    if (dlg.exec() == QDialog::Accepted) {
        customLibraryPath_ = dlg.libraryPath();
        appendVectorsToJedec_ = dlg.appendJedecFlag();
        usePldFilename_ = dlg.usePldNameFlag();
        actAppendJed_->setChecked(appendVectorsToJedec_);
        actUsePldName_->setChecked(usePldFilename_);
        refreshLibraryLabel();
        settingsChanged();
    }
}

void MainWindow::refreshLibraryLabel() {
    const QString lib = resolveActiveLibrary();
    libStatusLabel_->setText(lib.isEmpty()
                                 ? QStringLiteral("Lib: (not found)")
                                 : QStringLiteral("Lib: %1").arg(QFileInfo(lib).fileName()));
}

void MainWindow::onAssignSimulatorResults() {
    pushUndoSnapshot(QStringLiteral("Assign Simulator Results"));
    doc_.assignSimulatorResults();
    waveformEditor_->refreshAll();
    refreshCompanionTabs();
    refreshWindowTitle();
    statusBar()->showMessage(QStringLiteral("Assigned simulated outputs as expected test-vector values."), 4000);
}

void MainWindow::onApplySiSourceEdits() {
    SimDocument parsed;
    QString err;
    if (!parsed.loadSiText(siSourceEdit_->toPlainText(), doc_.filePath, &err)) {
        QMessageBox::warning(this, QStringLiteral(".SI Parse Error"), err);
        return;
    }
    pushUndoSnapshot(QStringLiteral("Edit .SI Source"));
    parsed.filePath = doc_.filePath;
    parsed.loadedFromWo = doc_.loadedFromWo;
    parsed.modified = true;
    doc_ = std::move(parsed);
    siEditorDirty_ = false;
    waveformEditor_->setDocument(&doc_);
    refreshWindowTitle();
    statusBar()->showMessage(QStringLiteral("Applied .SI source changes to waveform view."), 3000);
}

void MainWindow::onLaunchLinCupl() {
    const QString bin = findCliBinary(QStringLiteral("lincupl"));
    if (bin.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("LinCUPL IDE"),
                                 QStringLiteral("Sibling 'lincupl' binary was not found in bin directory."));
        return;
    }
    QStringList args;
    if (!doc_.filePath.isEmpty()) {
        const QString pld = SimDocument::findCompanionFile(doc_.filePath, QStringLiteral(".pld"));
        if (!pld.isEmpty()) args.append(pld);
    }
    QProcess::startDetached(bin, args);
}

void MainWindow::onShowQuickReference() {
    QMessageBox::information(
        this, QStringLiteral("LinSim Waveform States & Shortcuts"),
        QStringLiteral(
            "<b>CUPL Test-Vector States:</b><br>"
            "• <b>0 / 1</b> — Drive Input Low / High<br>"
            "• <b>L / H</b> — Test Expected Output Low / High<br>"
            "• <b>C / K</b> — Clock Pulse (0→1→0 rising / 1→0→1 falling)<br>"
            "• <b>Z</b> — Test High-Impedance (Tri-state)<br>"
            "• <b>X / N</b> — Don't Care / Untested<br>"
            "• <b>*</b> — Simulator Determined (CSIM computes actual output)<br>"
            "• <b>P</b> — Register Preload<br><br>"
            "<b>Interactive Editing:</b><br>"
            "• Select any cell or drag a range and press <b>0 1 C K P L H Z X *</b><br>"
            "• <b>Ctrl + Left-Click</b> sets <b>*</b> (Simulator Determined)<br>"
            "• <b>Double-Click</b> toggles scalar state or opens <b>Set Bus Value</b><br>"
            "• <b>Shift + Click</b> on ruler sets measurement reference marker<br>"
            "• <b>F9</b> runs CSIM and reloads simulated <code>.wo</code> &amp; <code>.so</code>"));
}

void MainWindow::onAbout() {
    QMessageBox::about(
        this, QStringLiteral("About LinSim"),
        QStringLiteral(
            "<h3>LinSim " LINCUPL_VERSION "</h3>"
            "<p>Native Qt6 Digital Waveform Simulator &amp; Test-Vector Editor for CUPL / Atmel SPLD &amp; CPLD designs.</p>"
            "<p>Supports <code>.SI</code> test vectors, "
            "<code>#WAVEFORM .WO</code> outputs, expandable multi-bit buses, and one-click <code>csim</code> simulation.</p>"));
}
