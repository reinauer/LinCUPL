#pragma once

#include <QDialog>
#include <QIcon>
#include <QList>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;

// One entry of the Tools menu (Tools > Add Tool..., Edit Tools...): an
// external program, typically a device programmer run on the design's
// JEDEC file.  Kept in QSettings as the array userTools.
struct UserTool {
    static constexpr int kMaxTools = 13;

    QString menuName;
    QString toolTip;         // toolbar tooltip; empty: the menu name
    QString command;         // program path or name on PATH; may hold placeholders
    QString arguments;       // split like a shell command line, then expanded
    QString workingDir;      // empty: the design's directory
    bool saveFirst = true;   // save all modified files before running
    bool captureOutput = true; // build log (async, stoppable); false: start detached
    bool onToolbar = false;

    static QList<UserTool> loadFromSettings();
    static void saveToSettings(const QList<UserTool> &tools);

    bool operator==(const UserTool &o) const;
};

// Values of the placeholders in a tool's command, arguments and working
// directory.
struct ToolContext {
    QString file;    // $(FILE)   the active tab's file
    QString design;  // $(DESIGN) the project's (or active) .PLD
    QString dir;     // $(DIR)    the design's directory (else the file's)
    QString name;    // $(NAME)   the design's base name (else the file's)
    QString device;  // $(DEVICE) the device override, else the design's Device
    QString jed;     // $(JED)    the design's JEDEC file
    QString si;      // $(SI)     the design's simulation input
    QString library; // $(LIB)    the device library
};

namespace UserTools {

// The placeholders with a short description each, for the dialogs.
QList<QPair<QString, QString>> placeholders();

// Replaces $(FILE), $(DESIGN), ... (any case); unknown names stay as they are.
QString expandPlaceholders(const QString &text, const ToolContext &ctx);

// Splits the argument template like a shell command line (quotes group
// words) and expands every word, so a path with spaces stays one argument.
QStringList expandArguments(const QString &argumentTemplate, const ToolContext &ctx);

// The program to start for a command: a name without a directory is looked
// up on PATH.
QString resolveProgram(const QString &command);

// The program's own icon where the platform has one (an application bundle
// or a Windows program), else the generic tool icon.
QIcon iconFor(const UserTool &tool);

} // namespace UserTools

// The fields of one tool, shared by Add Tool... and Edit Tools....
class UserToolEditor : public QWidget {
    Q_OBJECT

public:
    UserToolEditor(const ToolContext &ctx, QWidget *parent = nullptr);

    void setTool(const UserTool &tool);
    UserTool tool() const;
    // Empty if the tool can be saved, else what is missing.
    QString validationError() const;

signals:
    void changed();

private:
    void updatePreview();

    ToolContext m_ctx;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_tipEdit = nullptr;
    QLineEdit *m_commandEdit = nullptr;
    QLineEdit *m_argsEdit = nullptr;
    QLineEdit *m_dirEdit = nullptr;
    QCheckBox *m_saveFirstCheck = nullptr;
    QCheckBox *m_toolbarCheck = nullptr;
    QRadioButton *m_logRadio = nullptr;
    QRadioButton *m_detachedRadio = nullptr;
    QLabel *m_preview = nullptr;
};

// Tools > Add Tool...: one new tool.
class UserToolDialog : public QDialog {
    Q_OBJECT

public:
    UserToolDialog(const ToolContext &ctx, QWidget *parent = nullptr);
    UserTool tool() const;

private:
    UserToolEditor *m_editor = nullptr;
};

// Tools > Edit Tools...: the list of tools with their fields; tools can be
// added, removed and reordered.
class UserToolsDialog : public QDialog {
    Q_OBJECT

public:
    UserToolsDialog(const QList<UserTool> &tools, const ToolContext &ctx, QWidget *parent = nullptr);
    QList<UserTool> tools() const { return m_tools; }

private:
    void selectRow(int row);
    void storeCurrent();
    void refreshList();
    void updateButtons();

    QList<UserTool> m_tools;
    int m_currentRow = -1;
    QListWidget *m_list = nullptr;
    UserToolEditor *m_editor = nullptr;
    QPushButton *m_addBtn = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QPushButton *m_upBtn = nullptr;
    QPushButton *m_downBtn = nullptr;
};
