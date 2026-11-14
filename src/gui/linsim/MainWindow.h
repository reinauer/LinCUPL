#pragma once

#include "Dialogs.h"
#include "SimDocument.h"
#include "WaveformView.h"

#include <QMainWindow>

class QAction;
class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QScreen;
class QSplitter;
class QTabWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Every document has its own top-level window.  windows() lists them in
    // the order they were opened; newWindow() shows a new one that deletes
    // itself when closed, placed next to placeNear.
    static QList<MainWindow *> windows();
    static MainWindow *newWindow(MainWindow *placeNear = nullptr);
    // Opens path in the window that already shows it, in origin when that
    // holds no document, or else in a new window.  Returns the window, or
    // nullptr when the file cannot be read.
    static MainWindow *openInWindow(const QString &path, MainWindow *origin, QString *errorOut = nullptr);
    // Window > Cascade / Tile: arranges the visible LinSim windows on the
    // available area of screen.  Not offered where the platform does not let
    // applications place windows (Wayland).
    static void arrangeWindows(bool tile, QScreen *screen);
    static bool canArrangeWindows();
    // File > Save All: saves the document of every window with unsaved
    // changes.  Interactively an untitled document asks for a file name;
    // otherwise it cannot be saved and counts as a failure.
    static bool saveAll(bool interactive = true, QString *errorOut = nullptr);
    bool hasUnsavedChanges() const { return doc_.modified || siEditorDirty_; }

    // True while the window holds no document: an untitled, unchanged one.
    bool isUntouched() const;
    QString documentDisplayName() const;

    bool openDocument(const QString &path, QString *errorOut = nullptr);
    bool saveDocument(QString *errorOut = nullptr);
    bool saveDocumentAs(const QString &path, QString *errorOut = nullptr);

    bool runSimulation(bool compileFirst = false, bool interactive = true, QString *logOut = nullptr);

    SimDocument &document() { return doc_; }
    const SimDocument &document() const { return doc_; }
    WaveformEditorWidget *waveformEditor() const { return waveformEditor_; }

    QString soListingText() const;
    QString siSourceText() const;
    QTabWidget *viewTabs() const { return centralTabs_; }
    QPlainTextEdit *soListingEditor() const { return soListingView_; }
    QPlainTextEdit *siSourceEditor() const { return siSourceEdit_; }

    // Find in the current tab: the waveform editor searches for target, the
    // .SO and .SI tabs for text.  Returns whether something was found.
    bool find(const QString &needle, FindDialog::Target target, bool caseSensitive,
              bool wholeWords, bool backward);
    FindDialog *findDialog();

    // File > Print Colors: whether Print and PDF export are in colour.
    bool printColors() const;
    void setPrintColors(bool on);

    static QString findCliBinary(const QString &name);
    static QStringList discoverLibraryCandidates();
    QString resolveActiveLibrary() const;
    void setCustomLibraryPath(const QString &libPath) { customLibraryPath_ = libPath; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupUi();
    void setupMenusAndToolbar();
    // Settings are kept in QSettings as soon as they change.  The colours,
    // the library, the simulator flags and Print Colors are shared by all
    // windows; the view (grid, zoom, row height) is per window and the last
    // change is what a new window starts with.
    void loadSettings();
    void loadSharedSettings();
    void saveSettings() const;
    void settingsChanged();
    void refreshLibraryLabel();

    void adoptDocument(SimDocument &&doc);
    void rebuildWindowList();

    bool maybeSaveInteractive();
    void pushUndoSnapshot(const QString &description);
    void undo();
    void redo();

    void copySelectedVectors();
    void pasteVectors();

    void onFind();
    bool findNext(bool backward);
    void updateFindScope();

    void refreshWindowTitle();
    void refreshCompanionTabs();
    void refreshSignalDefinitionPane(int sigIndex);
    void populateDiagnosticsFromLog(const QString &logText);

    // Menu/Dialog handlers
    void onFileNew();
    void onFileOpen();
    bool onFileSave();
    bool onFileSaveAs();
    void onDesignProperties();
    void onExportPng();
    void onExportPdf();
    void onPrint();

    void onAddSignal();
    void onRenameSignal(int sigIndex);
    void onDeleteSignal(int sigIndex);
    void onMoveSignal(int fromIdx, int toIdx);
    void onSetBusValue(int sigIndex, int startVec, int endVec);
    void onAddVectorsDialog();
    void onEditVectorAnnotation(int vecIndex);

    void onGridSizeDialog();
    void onColorsDialog();
    void onSetLibraryDialog();
    void onAssignSimulatorResults();
    void onApplySiSourceEdits();
    void onLaunchLinCupl();
    void onShowQuickReference();
    void onAbout();

    SimDocument doc_;
    QVector<SimDocument> undoStack_;
    QVector<SimDocument> redoStack_;
    QVector<VectorRow> vectorClipboard_;

    QString customLibraryPath_;
    bool appendVectorsToJedec_ = false;
    bool usePldFilename_ = false;
    bool loadingSettings_ = false;
    bool simulationRunning_ = false;
    bool siEditorDirty_ = false;
    bool syncingSiEditor_ = false;

    QTabWidget *centralTabs_ = nullptr;
    QSplitter *waveSplitter_ = nullptr;
    WaveformEditorWidget *waveformEditor_ = nullptr;
    QPlainTextEdit *sigDefView_ = nullptr;
    QPlainTextEdit *soListingView_ = nullptr;
    QPlainTextEdit *siSourceEdit_ = nullptr;

    QDockWidget *simConsoleDock_ = nullptr;
    QPlainTextEdit *simConsoleOut_ = nullptr;
    QListWidget *diagList_ = nullptr;
    FindDialog *findDialog_ = nullptr;

    QLabel *cursorStatusLabel_ = nullptr;
    QLabel *libStatusLabel_ = nullptr;

    QAction *actUndo_ = nullptr;
    QAction *actRedo_ = nullptr;
    QAction *actFindNext_ = nullptr;
    QAction *actFindPrev_ = nullptr;
    QAction *actPrintColors_ = nullptr;
    QAction *actShowGrid_ = nullptr;
    QAction *actWideSignals_ = nullptr;
    QAction *actShowSigDef_ = nullptr;
    QAction *actAppendJed_ = nullptr;
    QAction *actUsePldName_ = nullptr;

    QMenu *windowMenu_ = nullptr;
    QAction *windowListEnd_ = nullptr;
    QList<QAction *> windowListActions_;
};
