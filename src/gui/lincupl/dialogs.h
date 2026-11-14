#pragma once

#include "device_library.h"
#include "error_catalog.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QRadioButton;
class QSpinBox;
class QTableWidget;
class QTextBrowser;
class QTreeWidget;
class CodeEditor;

// ---------------------------------------------------------------------------
// 1. Device Selection Browser (Options -> Devices...)
// ---------------------------------------------------------------------------
class DeviceBrowserDialog : public QDialog {
    Q_OBJECT

public:
    explicit DeviceBrowserDialog(DeviceLibrary *library,
                                 const QString &initialMnemonic,
                                 bool hasOpenPld,
                                 QWidget *parent = nullptr);

    QString selectedMnemonic() const { return m_selectedMnemonic; }
    QString selectedLibraryPath() const;
    bool shouldUpdatePldHeader() const;
    bool shouldSetCompilerOverride() const;

private slots:
    void onLibraryComboChanged(int index);
    void browseCustomLibrary();
    void refreshFilter();
    void onDeviceSelectionChanged();
    void runCbldInspect();

private:
    void populateLibraryCombo();
    void reloadCategoriesAndFilter();

    DeviceLibrary *m_library = nullptr;
    QString m_selectedMnemonic;

    QComboBox *m_libCombo = nullptr;
    QLabel *m_libSummaryLabel = nullptr;
    QListWidget *m_catList = nullptr;
    QComboBox *m_pkgCombo = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QTreeWidget *m_deviceTree = nullptr;
    QTextBrowser *m_detailsView = nullptr;
    QCheckBox *m_chkUpdatePld = nullptr;
    QCheckBox *m_chkOverride = nullptr;
};

// ---------------------------------------------------------------------------
// 2. New Design Wizard (File -> New Project / PLD... - frmNewDesign)
// ---------------------------------------------------------------------------
struct NewPinEntry {
    QString pinNumber; // optional pin/node number or range
    QString name;
    bool activeLow = false;
    QString comment;
};

struct NewDesignSpec {
    QString name = QStringLiteral("Design1");
    QString partno = QStringLiteral("00");
    QString date;
    QString revision = QStringLiteral("01");
    QString designer = QStringLiteral("Engineer");
    QString company = QStringLiteral("LinCUPL");
    QString assembly = QStringLiteral("None");
    QString location = QStringLiteral("None");
    QString device = QStringLiteral("g16v8a");
    QString saveFilePath;

    QList<NewPinEntry> inputs;
    QList<NewPinEntry> outputs;
    QList<NewPinEntry> pinnodes;
};

class NewDesignDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewDesignDialog(DeviceLibrary *library,
                             const QString &defaultDir,
                             QWidget *parent = nullptr);

    NewDesignSpec spec() const;
    static QString generatePldTemplate(const NewDesignSpec &spec);

private slots:
    void syncPinRows();
    void browseDevice();
    void browseSavePath();
    void updatePreview();

private:
    DeviceLibrary *m_library = nullptr;
    QString m_defaultDir;

    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_partnoEdit = nullptr;
    QLineEdit *m_dateEdit = nullptr;
    QLineEdit *m_revEdit = nullptr;
    QLineEdit *m_designerEdit = nullptr;
    QLineEdit *m_companyEdit = nullptr;
    QLineEdit *m_assemblyEdit = nullptr;
    QLineEdit *m_locationEdit = nullptr;
    QLineEdit *m_deviceEdit = nullptr;
    QLineEdit *m_filePathEdit = nullptr;

    QSpinBox *m_inCountSpin = nullptr;
    QSpinBox *m_outCountSpin = nullptr;
    QSpinBox *m_nodeCountSpin = nullptr;
    QTableWidget *m_pinTable = nullptr;
    QPlainTextEdit *m_previewEdit = nullptr;
};

// ---------------------------------------------------------------------------
// 3. New Simulation (.SI) Wizard (File -> New Simulation (.SI)...)
// ---------------------------------------------------------------------------
struct PldSignalInfo {
    QString name;
    bool activeLow = false;
    bool isOutput = false;
    bool isClock = false;
    bool include = true;
    QString pinNumber;
};

struct PldFieldInfo {
    QString name;
    QString expr; // e.g. "[Q3..0]" or "[clr,dir]"
    QStringList members;
};

struct PldDesignSummary {
    QString name = QStringLiteral("Design1");
    QString partno = QStringLiteral("00");
    QString date;
    QString revision = QStringLiteral("01");
    QString designer = QStringLiteral("Engineer");
    QString company = QStringLiteral("LinCUPL");
    QString assembly = QStringLiteral("None");
    QString location = QStringLiteral("None");
    QString device = QStringLiteral("g16v8a");

    QList<PldSignalInfo> signalsList;
    QList<PldFieldInfo> fields;
};

class NewSimDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewSimDialog(const QString &pldText,
                          const QString &defaultSiPath,
                          QWidget *parent = nullptr);

    QString siFilePath() const;
    QString generatedSiText() const;

    static PldDesignSummary parsePldForSimulation(const QString &pldText);
    static QString generateSiTemplate(const PldDesignSummary &summary, int vectorCount = 8);
    static QStringList expandSignalListExpr(const QString &expr);

private slots:
    void browseSavePath();
    void rebuildPreview();

private:
    PldDesignSummary currentSummaryFromUi() const;

    PldDesignSummary m_parsed;
    QLineEdit *m_savePathEdit = nullptr;
    QSpinBox *m_vecCountSpin = nullptr;
    QTableWidget *m_sigTable = nullptr;
    QPlainTextEdit *m_previewEdit = nullptr;
};

// ---------------------------------------------------------------------------
// 4. Searchable CuplErr.txt Viewer (Help -> Error Messages...)
// ---------------------------------------------------------------------------
class ErrorViewerDialog : public QDialog {
    Q_OBJECT

public:
    explicit ErrorViewerDialog(const ErrorCatalog *catalog,
                               const QString &initialCode = QString(),
                               QWidget *parent = nullptr);

    void selectErrorCode(const QString &code);

private slots:
    void refreshList();
    void onSelectionChanged();

private:
    const ErrorCatalog *m_catalog = nullptr;
    QComboBox *m_moduleCombo = nullptr;
    QComboBox *m_severityCombo = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QTreeWidget *m_tree = nullptr;
    QTextBrowser *m_detailBrowser = nullptr;
};

// ---------------------------------------------------------------------------
// 5. Backpin Utility Dialog (Utilities -> Backpin...)
// ---------------------------------------------------------------------------
struct BackpinRequest {
    QString sourcePldPath;
    QString deviceMnemonic;
    QString libraryPath;
    bool floatAllPins = false;     // -f
    bool pinNumbersOnly = false;   // -p
    QString customBackupFile;      // -b <file>
    QString logFile;               // -l <file>

    QStringList buildArgs() const;
};

class BackpinDialog : public QDialog {
    Q_OBJECT

public:
    explicit BackpinDialog(DeviceLibrary *library,
                           const QString &pldPath,
                           const QString &deviceMnemonic,
                           QWidget *parent = nullptr);

    BackpinRequest request() const;

private slots:
    void browsePld();
    void browseDevice();
    void browseLib();
    void updatePreview();

private:
    DeviceLibrary *m_library = nullptr;
    QLineEdit *m_pldEdit = nullptr;
    QLineEdit *m_devEdit = nullptr;
    QLineEdit *m_libEdit = nullptr;
    QRadioButton *m_rbAnnotate = nullptr;
    QRadioButton *m_rbFloat = nullptr;
    QCheckBox *m_chkPinOnly = nullptr;
    QLineEdit *m_bakEdit = nullptr;
    QLineEdit *m_logEdit = nullptr;
    QLineEdit *m_previewEdit = nullptr;
};

// ---------------------------------------------------------------------------
// 7. Binary Table Editor (Edit -> Binary Table Editor... - dlg_table)
// ---------------------------------------------------------------------------
class TableEditorDialog : public QDialog {
    Q_OBJECT

public:
    explicit TableEditorDialog(QWidget *parent = nullptr);

    QString generatedTable() const;
    void setSignals(const QString &inputsExpr, const QString &outputsExpr);
    void setOutputValue(int row, const QString &bits);

    static QString generateTable(const QStringList &inputs,
                                 const QStringList &outputs,
                                 const QStringList &outputValues);

private slots:
    void rebuildRows();
    void updatePreview();

private:
    QStringList inputSignals() const;
    QStringList outputSignals() const;

    QLineEdit *m_inEdit = nullptr;
    QLineEdit *m_outEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QLabel *m_status = nullptr;
};

// ---------------------------------------------------------------------------
// 8. CUPL Macros (Define / Create library / Reference - dlg_macro_*)
// ---------------------------------------------------------------------------
struct CuplMacroDef {
    QString name;
    QStringList params;
    QString sourceFile; // empty when defined inline in the current document
};

class MacroDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode { Define, Create, Reference };

    MacroDialog(Mode mode, const QString &currentText, const QString &baseDir,
                QWidget *parent = nullptr);

    // Text to insert at the cursor of the active editor.
    QString generatedText() const;
    // For Mode::Create: the library file written on accept (empty otherwise).
    QString writtenLibraryPath() const { return m_writtenLibrary; }

    static QList<CuplMacroDef> scanMacros(const QString &text, const QString &sourceFile = QString());
    static QList<CuplMacroDef> discoverMacros(const QString &currentText, const QString &baseDir);
    static QString formatDefinition(const QString &name, const QStringList &params, const QString &body);
    static QString formatReference(const CuplMacroDef &def, const QStringList &args);

private slots:
    void onMacroChanged(int index);
    void updatePreview();
    void browseLibrary();

private:
    Mode m_mode;
    QString m_currentText;
    QList<CuplMacroDef> m_macros;
    QString m_writtenLibrary;

    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_paramsEdit = nullptr;
    QPlainTextEdit *m_bodyEdit = nullptr;
    QLineEdit *m_libEdit = nullptr;
    QComboBox *m_macroCombo = nullptr;
    QTableWidget *m_argTable = nullptr;
    QCheckBox *m_chkInclude = nullptr;
    QPlainTextEdit *m_preview = nullptr;
};

// ---------------------------------------------------------------------------
// 6. Find / Replace Dialog (Edit -> Find / Replace...)
// ---------------------------------------------------------------------------
class FindReplaceDialog : public QDialog {
    Q_OBJECT

public:
    explicit FindReplaceDialog(std::function<CodeEditor *()> editorProvider,
                               QWidget *parent = nullptr);

    void focusFind(const QString &seedText = QString(), bool showReplace = false);

private slots:
    void onFindNext();
    void onFindPrev();
    void onReplaceOne();
    void onReplaceAll();

private:
    std::function<CodeEditor *()> m_editorProvider;
    QLineEdit *m_findEdit = nullptr;
    QLineEdit *m_replaceEdit = nullptr;
    QCheckBox *m_chkCase = nullptr;
    QCheckBox *m_chkWhole = nullptr;
    QLabel *m_statusLabel = nullptr;
};
