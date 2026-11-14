#include "user_tools.h"

#include "../common/action_icons.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// UserTool
// ---------------------------------------------------------------------------
bool UserTool::operator==(const UserTool &o) const {
    return menuName == o.menuName && toolTip == o.toolTip && command == o.command &&
           arguments == o.arguments && workingDir == o.workingDir && saveFirst == o.saveFirst &&
           captureOutput == o.captureOutput && onToolbar == o.onToolbar;
}

QList<UserTool> UserTool::loadFromSettings() {
    QSettings s;
    QList<UserTool> tools;
    const int n = qMin(s.beginReadArray(QStringLiteral("userTools")), kMaxTools);
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        UserTool t;
        t.menuName = s.value(QStringLiteral("name")).toString();
        t.toolTip = s.value(QStringLiteral("tooltip")).toString();
        t.command = s.value(QStringLiteral("command")).toString();
        t.arguments = s.value(QStringLiteral("arguments")).toString();
        t.workingDir = s.value(QStringLiteral("workingDir")).toString();
        t.saveFirst = s.value(QStringLiteral("saveFirst"), true).toBool();
        t.captureOutput = s.value(QStringLiteral("output")).toString() != QStringLiteral("detached");
        t.onToolbar = s.value(QStringLiteral("toolbar"), false).toBool();
        if (!t.menuName.isEmpty() && !t.command.isEmpty()) {
            tools << t;
        }
    }
    s.endArray();
    return tools;
}

void UserTool::saveToSettings(const QList<UserTool> &tools) {
    QSettings s;
    s.remove(QStringLiteral("userTools"));
    s.beginWriteArray(QStringLiteral("userTools"));
    for (int i = 0; i < tools.size() && i < kMaxTools; ++i) {
        const UserTool &t = tools.at(i);
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("name"), t.menuName);
        s.setValue(QStringLiteral("tooltip"), t.toolTip);
        s.setValue(QStringLiteral("command"), t.command);
        s.setValue(QStringLiteral("arguments"), t.arguments);
        s.setValue(QStringLiteral("workingDir"), t.workingDir);
        s.setValue(QStringLiteral("saveFirst"), t.saveFirst);
        s.setValue(QStringLiteral("output"), t.captureOutput ? QStringLiteral("log") : QStringLiteral("detached"));
        s.setValue(QStringLiteral("toolbar"), t.onToolbar);
    }
    s.endArray();
}

// ---------------------------------------------------------------------------
// Placeholders
// ---------------------------------------------------------------------------
namespace UserTools {

QList<QPair<QString, QString>> placeholders() {
    return {
        {QStringLiteral("$(FILE)"), QStringLiteral("file of the active tab")},
        {QStringLiteral("$(DESIGN)"), QStringLiteral("project design (.PLD)")},
        {QStringLiteral("$(DIR)"), QStringLiteral("directory of the design")},
        {QStringLiteral("$(NAME)"), QStringLiteral("design name without extension")},
        {QStringLiteral("$(DEVICE)"), QStringLiteral("device mnemonic")},
        {QStringLiteral("$(JED)"), QStringLiteral("JEDEC file of the design")},
        {QStringLiteral("$(SI)"), QStringLiteral("simulation input of the design")},
        {QStringLiteral("$(LIB)"), QStringLiteral("device library")},
    };
}

QString expandPlaceholders(const QString &text, const ToolContext &ctx) {
    static const QRegularExpression re(QStringLiteral("\\$\\(([A-Za-z]+)\\)"));
    QString out;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        const QString key = m.captured(1).toUpper();
        if (key == QLatin1String("FILE")) out += ctx.file;
        else if (key == QLatin1String("DESIGN")) out += ctx.design;
        else if (key == QLatin1String("DIR")) out += ctx.dir;
        else if (key == QLatin1String("NAME")) out += ctx.name;
        else if (key == QLatin1String("DEVICE")) out += ctx.device;
        else if (key == QLatin1String("JED")) out += ctx.jed;
        else if (key == QLatin1String("SI")) out += ctx.si;
        else if (key == QLatin1String("LIB")) out += ctx.library;
        else out += m.captured(0);
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

QStringList expandArguments(const QString &argumentTemplate, const ToolContext &ctx) {
    QStringList args;
    for (const QString &word : QProcess::splitCommand(argumentTemplate)) {
        args << expandPlaceholders(word, ctx);
    }
    return args;
}

QString resolveProgram(const QString &command) {
    if (command.isEmpty() || command.contains(QLatin1Char('/')) || command.contains(QLatin1Char('\\'))) {
        return command;
    }
    const QString found = QStandardPaths::findExecutable(command);
    return found.isEmpty() ? command : found;
}

QIcon iconFor(const UserTool &tool) {
    const QFileInfo fi(resolveProgram(tool.command));
    bool ownIcon = false;
#if defined(Q_OS_WIN)
    ownIcon = fi.isFile();
#elif defined(Q_OS_MACOS)
    ownIcon = fi.isDir() && fi.suffix().compare(QStringLiteral("app"), Qt::CaseInsensitive) == 0;
#endif
    if (ownIcon) {
        const QIcon icon = QFileIconProvider().icon(fi);
        if (!icon.isNull()) {
            return icon;
        }
    }
    return lincuplIcon(QStringLiteral("tool"));
}

} // namespace UserTools

// ---------------------------------------------------------------------------
// UserToolEditor
// ---------------------------------------------------------------------------
UserToolEditor::UserToolEditor(const ToolContext &ctx, QWidget *parent)
    : QWidget(parent), m_ctx(ctx) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *form = new QFormLayout();

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(QStringLiteral("e.g. Program with minipro"));
    form->addRow(QStringLiteral("Menu name:"), m_nameEdit);

    m_tipEdit = new QLineEdit(this);
    m_tipEdit->setPlaceholderText(QStringLiteral("(the menu name)"));
    form->addRow(QStringLiteral("Toolbar tooltip:"), m_tipEdit);

    auto *cmdRow = new QHBoxLayout();
    m_commandEdit = new QLineEdit(this);
    m_commandEdit->setPlaceholderText(QStringLiteral("program path, or a name found on PATH"));
    auto *cmdBrowse = new QPushButton(QStringLiteral("Browse..."), this);
    connect(cmdBrowse, &QPushButton::clicked, this, [this]() {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Select Program"),
                                                       QFileInfo(m_commandEdit->text()).absolutePath());
        if (!f.isEmpty()) m_commandEdit->setText(f);
    });
    cmdRow->addWidget(m_commandEdit, 1);
    cmdRow->addWidget(cmdBrowse);
    form->addRow(QStringLiteral("Command:"), cmdRow);

    auto *argRow = new QHBoxLayout();
    m_argsEdit = new QLineEdit(this);
    m_argsEdit->setPlaceholderText(QStringLiteral("e.g. -p $(DEVICE) -w \"$(JED)\""));
    auto *insertBtn = new QToolButton(this);
    insertBtn->setText(QStringLiteral("Insert"));
    insertBtn->setToolTip(QStringLiteral("Insert a placeholder"));
    insertBtn->setPopupMode(QToolButton::InstantPopup);
    auto *insertMenu = new QMenu(insertBtn);
    for (const auto &p : UserTools::placeholders()) {
        const QString name = p.first;
        insertMenu->addAction(QStringLiteral("%1\t%2").arg(name, p.second), this, [this, name]() {
            m_argsEdit->insert(name);
            m_argsEdit->setFocus();
        });
    }
    insertBtn->setMenu(insertMenu);
    argRow->addWidget(m_argsEdit, 1);
    argRow->addWidget(insertBtn);
    form->addRow(QStringLiteral("Arguments:"), argRow);

    auto *dirRow = new QHBoxLayout();
    m_dirEdit = new QLineEdit(this);
    m_dirEdit->setPlaceholderText(QStringLiteral("(the design's directory)"));
    auto *dirBrowse = new QPushButton(QStringLiteral("Browse..."), this);
    connect(dirBrowse, &QPushButton::clicked, this, [this]() {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Working Directory"),
                                                            m_dirEdit->text());
        if (!d.isEmpty()) m_dirEdit->setText(d);
    });
    dirRow->addWidget(m_dirEdit, 1);
    dirRow->addWidget(dirBrowse);
    form->addRow(QStringLiteral("Working directory:"), dirRow);
    root->addLayout(form);

    auto *runGroup = new QGroupBox(QStringLiteral("Running"), this);
    auto *runBox = new QVBoxLayout(runGroup);
    m_saveFirstCheck = new QCheckBox(QStringLiteral("Save all modified files first"), runGroup);
    m_logRadio = new QRadioButton(QStringLiteral("Show the output in the build log (can be stopped)"), runGroup);
    m_detachedRadio = new QRadioButton(QStringLiteral("Start detached, in its own window"), runGroup);
    m_toolbarCheck = new QCheckBox(QStringLiteral("Show on the toolbar"), runGroup);
    runBox->addWidget(m_saveFirstCheck);
    runBox->addWidget(m_logRadio);
    runBox->addWidget(m_detachedRadio);
    runBox->addWidget(m_toolbarCheck);
    root->addWidget(runGroup);

    QStringList help;
    for (const auto &p : UserTools::placeholders()) {
        help << QStringLiteral("<code>%1</code> %2").arg(p.first.toHtmlEscaped(), p.second.toHtmlEscaped());
    }
    auto *helpLabel = new QLabel(QStringLiteral("<small>Placeholders: %1</small>").arg(help.join(QStringLiteral(", "))), this);
    helpLabel->setWordWrap(true);
    root->addWidget(helpLabel);

    m_preview = new QLabel(this);
    m_preview->setWordWrap(true);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_preview);
    root->addStretch();

    for (QLineEdit *le : {m_nameEdit, m_tipEdit, m_commandEdit, m_argsEdit, m_dirEdit}) {
        connect(le, &QLineEdit::textChanged, this, [this]() {
            updatePreview();
            emit changed();
        });
    }
    for (QAbstractButton *b : std::initializer_list<QAbstractButton *>{
             m_saveFirstCheck, m_toolbarCheck, m_logRadio, m_detachedRadio}) {
        connect(b, &QAbstractButton::toggled, this, &UserToolEditor::changed);
    }
    setTool(UserTool{});
}

void UserToolEditor::setTool(const UserTool &tool) {
    m_nameEdit->setText(tool.menuName);
    m_tipEdit->setText(tool.toolTip);
    m_commandEdit->setText(tool.command);
    m_argsEdit->setText(tool.arguments);
    m_dirEdit->setText(tool.workingDir);
    m_saveFirstCheck->setChecked(tool.saveFirst);
    m_logRadio->setChecked(tool.captureOutput);
    m_detachedRadio->setChecked(!tool.captureOutput);
    m_toolbarCheck->setChecked(tool.onToolbar);
    updatePreview();
}

UserTool UserToolEditor::tool() const {
    UserTool t;
    t.menuName = m_nameEdit->text().trimmed();
    t.toolTip = m_tipEdit->text().trimmed();
    t.command = m_commandEdit->text().trimmed();
    t.arguments = m_argsEdit->text().trimmed();
    t.workingDir = m_dirEdit->text().trimmed();
    t.saveFirst = m_saveFirstCheck->isChecked();
    t.captureOutput = m_logRadio->isChecked();
    t.onToolbar = m_toolbarCheck->isChecked();
    return t;
}

QString UserToolEditor::validationError() const {
    if (m_nameEdit->text().trimmed().isEmpty()) {
        return QStringLiteral("Give the tool a menu name.");
    }
    if (m_commandEdit->text().trimmed().isEmpty()) {
        return QStringLiteral("Give the command (program) to run.");
    }
    return QString();
}

// The command line as it would run now, with the placeholders filled in.
void UserToolEditor::updatePreview() {
    const UserTool t = tool();
    if (t.command.isEmpty()) {
        m_preview->setText(QString());
        return;
    }
    QStringList words;
    words << UserTools::resolveProgram(UserTools::expandPlaceholders(t.command, m_ctx));
    for (const QString &a : UserTools::expandArguments(t.arguments, m_ctx)) {
        words << (a.contains(QLatin1Char(' ')) ? QStringLiteral("\"%1\"").arg(a) : a);
    }
    m_preview->setText(QStringLiteral("<small><b>Runs now:</b> <code>%1</code></small>")
                           .arg(words.join(QLatin1Char(' ')).toHtmlEscaped()));
}

// ---------------------------------------------------------------------------
// UserToolDialog
// ---------------------------------------------------------------------------
UserToolDialog::UserToolDialog(const ToolContext &ctx, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Add Tool"));
    resize(560, 440);
    auto *root = new QVBoxLayout(this);
    m_editor = new UserToolEditor(ctx, this);
    root->addWidget(m_editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        const QString err = m_editor->validationError();
        if (!err.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Add Tool"), err);
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

UserTool UserToolDialog::tool() const {
    return m_editor->tool();
}

// ---------------------------------------------------------------------------
// UserToolsDialog
// ---------------------------------------------------------------------------
UserToolsDialog::UserToolsDialog(const QList<UserTool> &tools, const ToolContext &ctx, QWidget *parent)
    : QDialog(parent), m_tools(tools) {
    setWindowTitle(QStringLiteral("Edit Tools"));
    resize(820, 460);
    auto *root = new QVBoxLayout(this);
    auto *body = new QHBoxLayout();

    auto *left = new QVBoxLayout();
    m_list = new QListWidget(this);
    m_list->setMinimumWidth(200);
    left->addWidget(m_list, 1);
    auto *btnRow = new QHBoxLayout();
    m_addBtn = new QPushButton(QStringLiteral("Add"), this);
    m_removeBtn = new QPushButton(QStringLiteral("Remove"), this);
    m_upBtn = new QPushButton(QStringLiteral("Up"), this);
    m_downBtn = new QPushButton(QStringLiteral("Down"), this);
    for (QPushButton *b : {m_addBtn, m_removeBtn, m_upBtn, m_downBtn}) {
        btnRow->addWidget(b);
    }
    left->addLayout(btnRow);
    body->addLayout(left);

    m_editor = new UserToolEditor(ctx, this);
    body->addWidget(m_editor, 1);
    root->addLayout(body, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        storeCurrent();
        for (int i = 0; i < m_tools.size(); ++i) {
            const UserTool &t = m_tools.at(i);
            if (t.menuName.isEmpty() || t.command.isEmpty()) {
                selectRow(i);
                QMessageBox::information(this, QStringLiteral("Edit Tools"), m_editor->validationError());
                return;
            }
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row != m_currentRow) {
            storeCurrent();
            selectRow(row);
        }
    });
    connect(m_editor, &UserToolEditor::changed, this, [this]() {
        if (m_currentRow >= 0 && m_currentRow < m_list->count()) {
            const QString name = m_editor->tool().menuName;
            m_list->item(m_currentRow)->setText(name.isEmpty() ? QStringLiteral("(unnamed)") : name);
        }
    });
    connect(m_addBtn, &QPushButton::clicked, this, [this]() {
        storeCurrent();
        UserTool t;
        t.menuName = QStringLiteral("New Tool");
        m_tools << t;
        refreshList();
        selectRow(m_tools.size() - 1);
    });
    connect(m_removeBtn, &QPushButton::clicked, this, [this]() {
        if (m_currentRow < 0) return;
        const int row = m_currentRow;
        m_tools.removeAt(row);
        m_currentRow = -1;
        refreshList();
        selectRow(qMin(row, m_tools.size() - 1));
    });
    auto move = [this](int delta) {
        storeCurrent();
        const int row = m_currentRow;
        const int to = row + delta;
        if (row < 0 || to < 0 || to >= m_tools.size()) return;
        m_tools.swapItemsAt(row, to);
        m_currentRow = -1;
        refreshList();
        selectRow(to);
    };
    connect(m_upBtn, &QPushButton::clicked, this, [move]() { move(-1); });
    connect(m_downBtn, &QPushButton::clicked, this, [move]() { move(1); });

    refreshList();
    selectRow(m_tools.isEmpty() ? -1 : 0);
}

void UserToolsDialog::refreshList() {
    const QSignalBlocker block(m_list);
    m_list->clear();
    for (const UserTool &t : m_tools) {
        m_list->addItem(new QListWidgetItem(UserTools::iconFor(t),
                                            t.menuName.isEmpty() ? QStringLiteral("(unnamed)") : t.menuName));
    }
}

// Shows tool `row` in the editor (-1: none).
void UserToolsDialog::selectRow(int row) {
    m_currentRow = row;
    {
        const QSignalBlocker block(m_list);
        m_list->setCurrentRow(row);
    }
    m_editor->setEnabled(row >= 0);
    m_editor->setTool(row >= 0 ? m_tools.at(row) : UserTool{});
    updateButtons();
}

void UserToolsDialog::storeCurrent() {
    if (m_currentRow >= 0 && m_currentRow < m_tools.size()) {
        m_tools[m_currentRow] = m_editor->tool();
    }
}

void UserToolsDialog::updateButtons() {
    m_addBtn->setEnabled(m_tools.size() < UserTool::kMaxTools);
    m_removeBtn->setEnabled(m_currentRow >= 0);
    m_upBtn->setEnabled(m_currentRow > 0);
    m_downBtn->setEnabled(m_currentRow >= 0 && m_currentRow < m_tools.size() - 1);
}
