// Utilities > Float All Pins and Nodes, Assign Pins and Pinnodes and Assign
// Pins Only: backpin run on the project's (or active) design with its
// device and the device library.
//
// backpin -f removes the pin and node numbers from the PIN / NODE / PINNODE
// statements so the fitter places the signals; backpin (or backpin -p for
// pins only) writes the numbers the fitter chose back into them.  backpin
// first copies the source to <design>.bak and writes the new source from
// that copy, so the .bak always holds the design as it was.  The editor
// takes the new text as one edit, so Edit > Undo brings the old text back
// as well.
//
// backpin reads the placements from <design>.plc (or <design>.ppn for
// fitter type 4 devices).  The ATF150x fitters report them in <design>.pin
// instead; when only that exists, a .plc is written from it for the run and
// removed afterwards.

#include "mainwindow.h"

#include "code_editor.h"
#include "tool_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBlock>

namespace {

// The entry of `dir` named `name`, compared without case (backpin opens its
// files the same way); empty if there is none.
QString findInDirNoCase(const QDir &dir, const QString &name) {
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
        if (fi.fileName().compare(name, Qt::CaseInsensitive) == 0) {
            return fi.absoluteFilePath();
        }
    }
    return QString();
}

// Converts the fitter's pin report, "<signal> : <number>" lines under a
// CHIP header, into backpin's placement file, "PIN <number> = <signal> ;"
// for device pins and "NODE <number> = <signal> ;" for the buried nodes
// numbered above them.
bool writePlcFromPinReport(const QString &pinPath, const QString &plcPath, int devicePins, QString *error) {
    QFile in(pinPath);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("Could not read %1.").arg(pinPath);
        return false;
    }
    static const QRegularExpression lineRe(QStringLiteral("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*:\\s*(\\d+)\\s*$"));
    QString out;
    int count = 0;
    for (const QString &line : QString::fromLatin1(in.readAll()).split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = lineRe.match(line);
        if (!m.hasMatch()) {
            continue;
        }
        const int number = m.captured(2).toInt();
        out += QStringLiteral("%1 %2 = %3 ;\n")
                   .arg(number > devicePins ? QStringLiteral("NODE") : QStringLiteral("PIN"))
                   .arg(number)
                   .arg(m.captured(1));
        ++count;
    }
    if (count == 0) {
        *error = QStringLiteral("%1 holds no pin placements.").arg(QFileInfo(pinPath).fileName());
        return false;
    }
    QSaveFile plc(plcPath);
    if (!plc.open(QIODevice::WriteOnly | QIODevice::Text) || plc.write(out.toLatin1()) < 0 || !plc.commit()) {
        *error = QStringLiteral("Could not write %1.").arg(plcPath);
        return false;
    }
    return true;
}

QString pinPlacementTitle(MainWindow::PinPlacement mode) {
    switch (mode) {
    case MainWindow::PinPlacement::FloatAll:
        return QStringLiteral("Float All Pins and Nodes");
    case MainWindow::PinPlacement::AssignPinsAndNodes:
        return QStringLiteral("Assign Pins and Pinnodes");
    case MainWindow::PinPlacement::AssignPinsOnly:
        return QStringLiteral("Assign Pins Only");
    }
    return QString();
}

} // namespace

// What a run would do, or why it cannot run.
MainWindow::PinPlacementPlan MainWindow::planPinPlacement(PinPlacement mode) const {
    PinPlacementPlan plan;
    const QString pld = activeDesignPldPath();
    if (pld.isEmpty()) {
        plan.error = QStringLiteral("Open a .PLD design or a project first.");
        return plan;
    }
    const QFileInfo pldInfo(pld);

    QString text;
    if (CodeEditor *ed = editorForPath(pld)) {
        text = ed->toPlainText();
    } else {
        QFile f(pld);
        if (f.open(QIODevice::ReadOnly)) {
            text = QString::fromLatin1(f.readAll());
        }
    }
    const QString device = m_deviceOverride.isEmpty() ? extractPldDeviceMnemonic(text) : m_deviceOverride;

    BackpinRequest &req = plan.request;
    req.sourcePldPath = pld;
    req.deviceMnemonic = device;
    req.libraryPath = m_deviceLib.libraryPath().isEmpty() ? ToolPaths::activeDeviceLibrary()
                                                          : m_deviceLib.libraryPath();
    if (mode == PinPlacement::FloatAll) {
        // Floating needs no device; backpin accepts "virtual" for any design.
        req.floatAllPins = true;
        if (req.deviceMnemonic.isEmpty()) {
            req.deviceMnemonic = QStringLiteral("virtual");
        }
        return plan;
    }
    req.pinNumbersOnly = (mode == PinPlacement::AssignPinsOnly);

    if (device.isEmpty()) {
        plan.error = QStringLiteral("%1 has no Device statement and no device is chosen in "
                                    "Options > Devices..., so the fitter's pin placements cannot be read.")
                         .arg(pldInfo.fileName());
        return plan;
    }
    const DeviceInfo info = m_deviceLib.findDevice(device);
    if (!info.isValid()) {
        plan.error = QStringLiteral("Device %1 is not in the device library %2.")
                         .arg(device, QFileInfo(req.libraryPath).fileName());
        return plan;
    }
    if (info.fitterType <= 1) {
        plan.error = QStringLiteral("%1 has no fitter: the pins of a design for it are the ones its PIN "
                                    "statements give.\n\nAssign Pins writes the pin numbers a fitter chose for "
                                    "a CPLD (such as an ATF150x) back into the design.")
                         .arg(info.mnemonic);
        return plan;
    }

    const QString stem = pldInfo.completeBaseName();
    const QString ext = (info.fitterType == 4) ? QStringLiteral("ppn") : QStringLiteral("plc");
    plan.placementFile = findInDirNoCase(pldInfo.absoluteDir(), stem + QLatin1Char('.') + ext);
    if (plan.placementFile.isEmpty() && ext == QStringLiteral("plc")) {
        const QString pinReport = findCompanionFile(pld, QStringLiteral("pin"));
        if (!pinReport.isEmpty()) {
            plan.placementFile = pinReport;
            plan.generatedPlc = pldInfo.absoluteDir().filePath(stem + QStringLiteral(".plc"));
        }
    }
    if (plan.placementFile.isEmpty()) {
        plan.error = QStringLiteral("No pin placements were found for %1.\n\n"
                                    "The fitter writes the pins it chose (%2.%3%4) when the design is "
                                    "compiled for %5. Compile it with Run > Device Dependent Compile first.")
                         .arg(pldInfo.fileName(), stem, ext,
                              ext == QStringLiteral("plc") ? QStringLiteral(" or %1.pin").arg(stem) : QString(),
                              info.mnemonic);
        return plan;
    }
    plan.stale = QFileInfo(plan.placementFile).lastModified() < pldInfo.lastModified();
    return plan;
}

void MainWindow::startPinPlacement(PinPlacement mode) {
    const QString title = pinPlacementTitle(mode);
    const QString pld = activeDesignPldPath();
    if (CodeEditor *ed = editorForPath(pld); ed && ed->document()->isModified()) {
        if (!saveTab(m_tabWidget->indexOf(ed), false)) {
            return;
        }
    }

    PinPlacementPlan plan = planPinPlacement(mode);
    if (!plan.error.isEmpty()) {
        QMessageBox::information(this, title, plan.error);
        return;
    }
    if (ToolPaths::findExecutable(QStringLiteral("backpin")).isEmpty()) {
        QMessageBox::warning(this, title, QStringLiteral("Could not find the 'backpin' program."));
        return;
    }
    if (plan.stale &&
        QMessageBox::question(this, title,
                              QStringLiteral("The pin placements in %1 are older than %2, which was changed "
                                             "after it was last compiled.\n\nAssign them anyway?")
                                  .arg(QFileInfo(plan.placementFile).fileName(), QFileInfo(pld).fileName()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (!plan.generatedPlc.isEmpty()) {
        QString err;
        const int pins = m_deviceLib.findDevice(plan.request.deviceMnemonic).pins;
        if (!writePlcFromPinReport(plan.placementFile, plan.generatedPlc, pins, &err)) {
            QMessageBox::warning(this, title, err);
            return;
        }
        m_generatedPlcPath = plan.generatedPlc;
    }

    startBackpin(plan.request);
    if (m_activeJob != JobKind::Backpin) {
        if (!m_generatedPlcPath.isEmpty()) {
            QFile::remove(m_generatedPlcPath);
            m_generatedPlcPath.clear();
        }
        return;
    }
    if (!plan.generatedPlc.isEmpty()) {
        m_buildLogEdit->appendPlainText(QStringLiteral("=== Pin placements from %1\n")
                                            .arg(QFileInfo(plan.placementFile).fileName()));
    }
    statusBar()->showMessage(QStringLiteral("%1: %2...").arg(title, QFileInfo(pld).fileName()));
}

bool MainWindow::backpinKeptDesign(const QString &before, const QString &after) {
    // Everything between the keyword and the '=' of a PIN, PINNODE or NODE
    // statement is where backpin writes or removes numbers.
    static const QRegularExpression numbers(QStringLiteral("\\b(pin|pinnode|node)\\b[^=;]*="),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression space(QStringLiteral("\\s+"));
    auto invariant = [](QString text) {
        text.replace(numbers, QStringLiteral("\\1="));
        text.remove(space);
        return text;
    };
    return invariant(before) == invariant(after);
}

// After any backpin run: drop the .plc written for it and show the new
// source in the editor.  backpin leaves the source unchanged when it stops
// at a statement it cannot rewrite; should the source it leaves differ from
// the one before in more than pin and node numbers (another backpin that
// writes only part of the design), the design is put back as it was.
void MainWindow::finishBackpin() {
    if (!m_generatedPlcPath.isEmpty()) {
        QFile::remove(m_generatedPlcPath);
        m_generatedPlcPath.clear();
    }
    if (m_activeJobSourcePath.isEmpty()) {
        return;
    }
    if (m_backpinHaveSource) {
        m_backpinHaveSource = false;
        QByteArray after;
        QFile f(m_activeJobSourcePath);
        if (f.open(QIODevice::ReadOnly)) {
            after = f.readAll();
            f.close();
        }
        if (!backpinKeptDesign(QString::fromLatin1(m_backpinSourceBefore), QString::fromLatin1(after))) {
            QSaveFile restore(m_activeJobSourcePath);
            const bool restored = restore.open(QIODevice::WriteOnly) &&
                                  restore.write(m_backpinSourceBefore) == m_backpinSourceBefore.size() &&
                                  restore.commit();
            QStringList reasons;
            static const QRegularExpression bpError(QStringLiteral("^\\[\\d{4}bp\\].*$"),
                                                    QRegularExpression::MultilineOption);
            for (auto it = bpError.globalMatch(m_accumulatedOutput); it.hasNext();) {
                reasons << it.next().captured(0).trimmed();
            }
            const QString name = QFileInfo(m_activeJobSourcePath).fileName();
            const QString text =
                restored
                    ? QStringLiteral("backpin could not rewrite %1 and wrote only part of it%2.\n\n"
                                     "%1 has been restored as it was; nothing was changed.")
                          .arg(name, reasons.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reasons.join(QStringLiteral("; "))))
                    : QStringLiteral("backpin could not rewrite %1 and wrote only part of it, and %1 could not be "
                                     "restored.  The design as it was is in the backup file %2.bak.")
                          .arg(name, QFileInfo(m_activeJobSourcePath).completeBaseName());
            m_lastRunOk = false;
            m_buildLogEdit->appendPlainText(QStringLiteral("=== ") + QString(text).replace(QStringLiteral("\n\n"), QStringLiteral("\n=== ")));
            if (CodeEditor *ed = editorForPath(m_activeJobSourcePath); ed && restored) {
                ed->document()->setModified(false);
            }
            auto *box = new QMessageBox(QMessageBox::Warning, QStringLiteral("backpin"), text, QMessageBox::Ok, this);
            box->setObjectName(QStringLiteral("backpinRestoredMessage"));
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
            return;
        }
    }
    reloadEditorKeepingUndo(m_activeJobSourcePath);
    const QFileInfo fi(m_activeJobSourcePath);
    m_buildLogEdit->appendPlainText(
        QStringLiteral("=== The source as it was before is kept in the backup file %1.bak; "
                       "Edit > Undo also brings it back.")
            .arg(fi.completeBaseName()));
}

// Replaces the editor's text with the file's as a single edit, so that
// Undo returns to the text before, and keeps the cursor line and scroll
// position.  Opens the file if no editor shows it.
void MainWindow::reloadEditorKeepingUndo(const QString &path) {
    CodeEditor *ed = editorForPath(path);
    if (!ed) {
        openFile(path, true);
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QString text = ErrorCatalog::normalizeConsoleText(QString::fromLatin1(f.readAll()));
    m_tabWidget->setCurrentWidget(ed);
    if (text != ed->toPlainText()) {
        const int line = ed->textCursor().blockNumber();
        const int scroll = ed->verticalScrollBar()->value();
        QTextCursor c(ed->document());
        c.beginEditBlock();
        c.select(QTextCursor::Document);
        c.insertText(text);
        c.endEditBlock();
        QTextBlock block = ed->document()->findBlockByNumber(qMin(line, ed->blockCount() - 1));
        ed->setTextCursor(QTextCursor(block));
        ed->verticalScrollBar()->setValue(scroll);
    }
    ed->document()->setModified(false);
    updateCompanionSidebar();
}
