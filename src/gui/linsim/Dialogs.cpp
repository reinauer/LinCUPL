#include "Dialogs.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>

DesignPropertiesDialog::DesignPropertiesDialog(const SimHeader &hdr, BusRadix base, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Design Properties"));
    setMinimumWidth(420);

    auto *mainLay = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    nameEdit_ = new QLineEdit(hdr.name, this);
    partNoEdit_ = new QLineEdit(hdr.partNo, this);
    dateEdit_ = new QLineEdit(hdr.date, this);
    revEdit_ = new QLineEdit(hdr.revision, this);
    designerEdit_ = new QLineEdit(hdr.designer, this);
    companyEdit_ = new QLineEdit(hdr.company, this);
    assemblyEdit_ = new QLineEdit(hdr.assembly, this);
    locationEdit_ = new QLineEdit(hdr.location, this);
    deviceEdit_ = new QLineEdit(hdr.device, this);

    baseCombo_ = new QComboBox(this);
    baseCombo_->addItem(QStringLiteral("Hexadecimal (Default)"), static_cast<int>(BusRadix::Hex));
    baseCombo_->addItem(QStringLiteral("Octal (BASE: octal;)"), static_cast<int>(BusRadix::Octal));
    baseCombo_->addItem(QStringLiteral("Decimal (BASE: decimal;)"), static_cast<int>(BusRadix::Decimal));
    int bIdx = baseCombo_->findData(static_cast<int>(base));
    if (bIdx >= 0) baseCombo_->setCurrentIndex(bIdx);

    form->addRow(QStringLiteral("Name:"), nameEdit_);
    form->addRow(QStringLiteral("PartNo:"), partNoEdit_);
    form->addRow(QStringLiteral("Date:"), dateEdit_);
    form->addRow(QStringLiteral("Revision:"), revEdit_);
    form->addRow(QStringLiteral("Designer:"), designerEdit_);
    form->addRow(QStringLiteral("Company:"), companyEdit_);
    form->addRow(QStringLiteral("Assembly:"), assemblyEdit_);
    form->addRow(QStringLiteral("Location:"), locationEdit_);
    form->addRow(QStringLiteral("Device:"), deviceEdit_);
    form->addRow(QStringLiteral("Bus File Base:"), baseCombo_);
    mainLay->addLayout(form);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLay->addWidget(btns);
}

SimHeader DesignPropertiesDialog::headerResult() const {
    SimHeader h;
    h.name = nameEdit_->text().trimmed();
    h.partNo = partNoEdit_->text().trimmed();
    h.date = dateEdit_->text().trimmed();
    h.revision = revEdit_->text().trimmed();
    h.designer = designerEdit_->text().trimmed();
    h.company = companyEdit_->text().trimmed();
    h.assembly = assemblyEdit_->text().trimmed();
    h.location = locationEdit_->text().trimmed();
    h.device = deviceEdit_->text().trimmed();
    return h;
}

BusRadix DesignPropertiesDialog::baseResult() const {
    return static_cast<BusRadix>(baseCombo_->currentData().toInt());
}

AddSignalDialog::AddSignalDialog(const SimDocument &doc, QWidget *parent)
    : QDialog(parent), doc_(doc) {
    setWindowTitle(QStringLiteral("Add Signal / Bus"));
    setMinimumWidth(460);

    auto *mainLay = new QVBoxLayout(this);

    QSet<QString> usedPins;
    QSet<QString> usedNames;
    for (const SignalItem &s : doc.signalsList) {
        usedNames.insert(s.name.toLower());
        for (const QString &m : s.members) usedPins.insert(m.toLower());
    }

    availableList_ = new QListWidget(this);
    availableList_->setMaximumHeight(130);

    for (auto fit = doc.fields.begin(); fit != doc.fields.end(); ++fit) {
        if (!usedNames.contains(fit.key())) {
            auto *it = new QListWidgetItem(
                QStringLiteral("[FIELD] %1 = [%2]").arg(fit->name, fit->members.join(QLatin1Char(','))),
                availableList_);
            it->setData(Qt::UserRole, QStringLiteral("F:") + fit->name);
        }
    }
    for (auto pit = doc.pinMeta.begin(); pit != doc.pinMeta.end(); ++pit) {
        if (!usedPins.contains(pit.key())) {
            const QString tag = pit->isOutput ? QStringLiteral("OUT") : QStringLiteral("IN");
            const QString pnum = pit->pinNumber.isEmpty() ? QString() : QStringLiteral(" (Pin %1)").arg(pit->pinNumber);
            auto *it = new QListWidgetItem(
                QStringLiteral("[%1] %2%3%4")
                    .arg(tag, pit->activeLowInDesign ? QStringLiteral("!") : QString(), pit->name, pnum),
                availableList_);
            it->setData(Qt::UserRole, QStringLiteral("P:") + pit->name);
        }
    }

    if (availableList_->count() > 0) {
        mainLay->addWidget(new QLabel(QStringLiteral("Available Design Signals / Fields (click to populate):"), this));
        mainLay->addWidget(availableList_);
    } else {
        availableList_->hide();
    }

    auto *form = new QFormLayout();
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(QStringLiteral("e.g. clk, !oe, Q3..Q0, or bus_name"));

    activeLowCheck_ = new QCheckBox(QStringLiteral("Active-Low Polarity (!) in ORDER:"), this);
    isBusCheck_ = new QCheckBox(QStringLiteral("Named Multi-Bit FIELD Bus"), this);

    membersEdit_ = new QLineEdit(this);
    membersEdit_->setPlaceholderText(QStringLiteral("e.g. D7..D0 or Q3, Q2, Q1, Q0"));
    membersEdit_->setEnabled(false);

    posCombo_ = new QComboBox(this);
    posCombo_->addItem(QStringLiteral("Append at End of ORDER:"));
    posCombo_->addItem(QStringLiteral("Insert Before Selected Signal"));

    form->addRow(QStringLiteral("Signal / Range Name:"), nameEdit_);
    form->addRow(QString(), activeLowCheck_);
    form->addRow(QString(), isBusCheck_);
    form->addRow(QStringLiteral("Bus Members (MSB..LSB):"), membersEdit_);
    form->addRow(QStringLiteral("Insert Position:"), posCombo_);
    mainLay->addLayout(form);

    connect(isBusCheck_, &QCheckBox::toggled, membersEdit_, &QWidget::setEnabled);

    connect(availableList_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString code = item->data(Qt::UserRole).toString();
        if (code.startsWith(QStringLiteral("F:"))) {
            const QString fname = code.mid(2);
            auto fit = doc_.fields.find(fname.toLower());
            if (fit != doc_.fields.end()) {
                nameEdit_->setText(fit->name);
                isBusCheck_->setChecked(true);
                membersEdit_->setText(fit->members.join(QStringLiteral(", ")));
            }
        } else if (code.startsWith(QStringLiteral("P:"))) {
            const QString pname = code.mid(2);
            auto pit = doc_.pinMeta.find(pname.toLower());
            if (pit != doc_.pinMeta.end()) {
                nameEdit_->setText(pit->name);
                activeLowCheck_->setChecked(pit->activeLowInDesign);
                isBusCheck_->setChecked(false);
                membersEdit_->clear();
            }
        }
    });

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, [this]() {
        SignalItem dummy;
        bool before = false;
        QString err;
        if (!buildSignalItem(dummy, before, &err)) {
            QMessageBox::warning(this, QStringLiteral("Invalid Signal"), err);
            return;
        }
        accept();
    });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLay->addWidget(btns);
}

bool AddSignalDialog::buildSignalItem(SignalItem &outItem, bool &insertBeforeSelected, QString *errorOut) const {
    insertBeforeSelected = (posCombo_->currentIndex() == 1);
    QString rawName = nameEdit_->text().trimmed();
    bool actLow = activeLowCheck_->isChecked();
    if (rawName.startsWith(QLatin1Char('!'))) {
        actLow = true;
        rawName = rawName.mid(1).trimmed();
    }
    if (rawName.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Please enter a signal name or range (e.g. Q3..Q0).");
        return false;
    }

    if (rawName.contains(QStringLiteral(".."))) {
        QStringList members;
        bool rngLow = false;
        QString cleanRng;
        if (!SimDocument::expandRangeToken(rawName, members, &rngLow, &cleanRng)) {
            if (errorOut) *errorOut = QStringLiteral("Invalid range syntax '%1'.").arg(rawName);
            return false;
        }
        outItem.kind = SignalItem::Kind::RangeBus;
        outItem.name = cleanRng;
        outItem.activeLow = actLow || rngLow;
        outItem.members = members;
        return true;
    }

    if (isBusCheck_->isChecked()) {
        QStringList members = SimDocument::expandBracketList(membersEdit_->text());
        for (QString &m : members) {
            if (m.startsWith(QLatin1Char('!'))) m = m.mid(1).trimmed();
        }
        if (members.size() < 2) {
            if (errorOut) *errorOut = QStringLiteral("Please enter at least 2 member pins for bus '%1'.").arg(rawName);
            return false;
        }
        outItem.kind = SignalItem::Kind::FieldBus;
        outItem.name = rawName;
        outItem.activeLow = actLow;
        outItem.members = members;
        return true;
    }

    // Check if rawName matches an existing design FIELD automatically
    auto fit = doc_.fields.find(rawName.toLower());
    if (fit != doc_.fields.end() && fit->members.size() > 1) {
        outItem.kind = SignalItem::Kind::FieldBus;
        outItem.name = fit->name;
        outItem.activeLow = actLow;
        outItem.members = fit->members;
        return true;
    }

    outItem.kind = SignalItem::Kind::Scalar;
    outItem.name = rawName;
    outItem.activeLow = actLow;
    outItem.members = {rawName};
    return true;
}

AddVectorDialog::AddVectorDialog(int currentVec, int totalVecs, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Add Vectors"));
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    countSpin_ = new QSpinBox(this);
    countSpin_->setRange(1, 4096);
    countSpin_->setValue(1);

    posCombo_ = new QComboBox(this);
    posCombo_->addItem(QStringLiteral("Append at End (after #%1)").arg(totalVecs));
    posCombo_->addItem(QStringLiteral("Insert Before Vector #%1").arg(currentVec + 1));
    posCombo_->addItem(QStringLiteral("Insert After Vector #%1").arg(currentVec + 1));

    initCombo_ = new QComboBox(this);
    initCombo_->addItem(QStringLiteral("Copy Neighbor Vector States"));
    initCombo_->addItem(QStringLiteral("Default (Inputs = 0, Outputs = * Simulator Determined)"));
    initCombo_->addItem(QStringLiteral("All Don't Care (X)"));

    form->addRow(QStringLiteral("Number of Vectors:"), countSpin_);
    form->addRow(QStringLiteral("Insert Location:"), posCombo_);
    form->addRow(QStringLiteral("Initial States:"), initCombo_);
    lay->addLayout(form);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

int AddVectorDialog::countResult() const { return countSpin_->value(); }

AddVectorDialog::Position AddVectorDialog::positionResult() const {
    if (posCombo_->currentIndex() == 1) return Position::InsertBeforeCursor;
    if (posCombo_->currentIndex() == 2) return Position::InsertAfterCursor;
    return Position::AppendEnd;
}

AddVectorDialog::InitMode AddVectorDialog::initModeResult() const {
    if (initCombo_->currentIndex() == 1) return InitMode::DefaultIn0OutSim;
    if (initCombo_->currentIndex() == 2) return InitMode::AllDontCare;
    return InitMode::CopyNeighbor;
}

SetBusValueDialog::SetBusValueDialog(const SimDocument &doc, int sigIdx, int startVec, int endVec,
                                     QWidget *parent)
    : QDialog(parent) {
    const SignalItem &sig = doc.signalsList[sigIdx];
    setWindowTitle(QStringLiteral("Set Bus Value — %1 [%2 bits]").arg(sig.displayLabel()).arg(sig.width()));
    setMinimumWidth(400);

    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    bool cleanNum = false;
    QString curDigits = doc.formatBusValue(sig, startVec, sig.displayRadix, false, &cleanNum);
    valEdit_ = new QLineEdit(cleanNum ? curDigits : QStringLiteral("0"), this);
    valEdit_->selectAll();

    radixCombo_ = new QComboBox(this);
    radixCombo_->addItem(QStringLiteral("Hexadecimal"), static_cast<int>(BusRadix::Hex));
    radixCombo_->addItem(QStringLiteral("Decimal"), static_cast<int>(BusRadix::Decimal));
    radixCombo_->addItem(QStringLiteral("Octal"), static_cast<int>(BusRadix::Octal));
    radixCombo_->addItem(QStringLiteral("Binary"), static_cast<int>(BusRadix::Binary));
    int rIdx = radixCombo_->findData(static_cast<int>(sig.displayRadix));
    if (rIdx >= 0) radixCombo_->setCurrentIndex(rIdx);

    driveCombo_ = new QComboBox(this);
    driveCombo_->addItem(QStringLiteral("Drive Input (0 / 1)"), false);
    driveCombo_->addItem(QStringLiteral("Test Expected Output (L / H)"), true);
    driveCombo_->setCurrentIndex(doc.isSignalOutput(sig) ? 1 : 0);

    const int nVecs = qMax(1, doc.vectors.size());
    startSpin_ = new QSpinBox(this);
    startSpin_->setRange(1, nVecs);
    startSpin_->setValue(startVec + 1);

    endSpin_ = new QSpinBox(this);
    endSpin_->setRange(1, nVecs);
    endSpin_->setValue(endVec + 1);

    form->addRow(QStringLiteral("Value:"), valEdit_);
    form->addRow(QStringLiteral("Radix:"), radixCombo_);
    form->addRow(QStringLiteral("Pin Mode:"), driveCombo_);
    form->addRow(QStringLiteral("From Vector:"), startSpin_);
    form->addRow(QStringLiteral("To Vector:"), endSpin_);
    lay->addLayout(form);

    // Quick special-state buttons
    auto *quickLay = new QHBoxLayout();
    quickLay->addWidget(new QLabel(QStringLiteral("Special:"), this));
    for (const auto &pair : {qMakePair(QStringLiteral("All X"), QStringLiteral("X")),
                             qMakePair(QStringLiteral("High-Z"), QStringLiteral("Z")),
                             qMakePair(QStringLiteral("Sim (*)"), QStringLiteral("*"))}) {
        auto *b = new QPushButton(pair.first, this);
        connect(b, &QPushButton::clicked, this, [this, code = pair.second]() {
            valEdit_->setText(code);
            accept();
        });
        quickLay->addWidget(b);
    }
    quickLay->addStretch();
    lay->addLayout(quickLay);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

QString SetBusValueDialog::valueText() const { return valEdit_->text().trimmed(); }
BusRadix SetBusValueDialog::radix() const { return static_cast<BusRadix>(radixCombo_->currentData().toInt()); }
bool SetBusValueDialog::driveAsOutput() const { return driveCombo_->currentData().toBool(); }
int SetBusValueDialog::startVector() const { return startSpin_->value() - 1; }
int SetBusValueDialog::endVector() const { return endSpin_->value() - 1; }

GridSizeDialog::GridSizeDialog(int cellW, int rowH, bool showGrid, bool wideSignals, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Grid Size & Row Spacing"));
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    cellWSpin_ = new QSpinBox(this);
    cellWSpin_->setRange(10, 160);
    cellWSpin_->setSuffix(QStringLiteral(" px"));
    cellWSpin_->setValue(cellW);

    rowHSpin_ = new QSpinBox(this);
    rowHSpin_->setRange(20, 96);
    rowHSpin_->setSuffix(QStringLiteral(" px"));
    rowHSpin_->setValue(rowH);

    gridCheck_ = new QCheckBox(QStringLiteral("Display Waveform Grid Lines"), this);
    gridCheck_->setChecked(showGrid);

    wideCheck_ = new QCheckBox(QStringLiteral("Wide Signal Rows"), this);
    wideCheck_->setChecked(wideSignals);
    connect(wideCheck_, &QCheckBox::toggled, this, [this](bool on) {
        rowHSpin_->setValue(on ? 44 : 30);
    });

    form->addRow(QStringLiteral("Horizontal Vector Width:"), cellWSpin_);
    form->addRow(QStringLiteral("Vertical Row Height:"), rowHSpin_);
    form->addRow(QString(), gridCheck_);
    form->addRow(QString(), wideCheck_);
    lay->addLayout(form);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

int GridSizeDialog::cellWidth() const { return cellWSpin_->value(); }
int GridSizeDialog::rowHeight() const { return rowHSpin_->value(); }
bool GridSizeDialog::showGrid() const { return gridCheck_->isChecked(); }
bool GridSizeDialog::wideSignals() const { return wideCheck_->isChecked(); }

WaveformColorsDialog::WaveformColorsDialog(const WaveformColorTheme &initial, QWidget *parent)
    : QDialog(parent), theme_(initial) {
    setWindowTitle(QStringLiteral("Waveform Colors"));
    setMinimumWidth(460);

    auto *lay = new QVBoxLayout(this);
    auto *grid = new QGridLayout();

    struct ItemSpec {
        const char *label;
        QColor WaveformColorTheme::*member;
    };
    static const ItemSpec specs[] = {
        {"0 - Input Low", &WaveformColorTheme::state0},
        {"1 - Input High", &WaveformColorTheme::state1},
        {"L - Output Low", &WaveformColorTheme::stateL},
        {"H - Output High", &WaveformColorTheme::stateH},
        {"C - Clock (0-1-0)", &WaveformColorTheme::stateC},
        {"K - Clock (1-0-1)", &WaveformColorTheme::stateK},
        {"Z - High Impedance", &WaveformColorTheme::stateZ},
        {"X - Don't Care", &WaveformColorTheme::stateX},
        {"* - Simulator Determined", &WaveformColorTheme::stateSim},
        {"P - Register Preload", &WaveformColorTheme::stateP},
        {"Bus Envelope", &WaveformColorTheme::busStroke},
        {"Error Highlight", &WaveformColorTheme::errorBorder},
        {"Canvas Background", &WaveformColorTheme::canvasBg},
        {"Grid Lines", &WaveformColorTheme::gridLine},
    };

    int idx = 0;
    for (const auto &sp : specs) {
        int r = idx / 2;
        int c = (idx % 2) * 2;
        grid->addWidget(new QLabel(QString::fromLatin1(sp.label), this), r, c);
        auto *btn = new QPushButton(this);
        btn->setFixedWidth(78);
        swatches_.append(Swatch{sp.member, btn});
        connect(btn, &QPushButton::clicked, this, [this, m = sp.member, lbl = QString::fromLatin1(sp.label)]() {
            QColor chosen = QColorDialog::getColor(theme_.*m, this, lbl);
            if (chosen.isValid()) {
                theme_.*m = chosen;
                refreshButtons();
            }
        });
        grid->addWidget(btn, r, c + 1);
        idx++;
    }
    lay->addLayout(grid);

    auto *presetLay = new QHBoxLayout();
    auto *darkBtn = new QPushButton(QStringLiteral("Dark Logic Analyzer Preset"), this);
    auto *lightBtn = new QPushButton(QStringLiteral("Classic WinSim Light Preset"), this);
    connect(darkBtn, &QPushButton::clicked, this, [this]() {
        theme_ = WaveformColorTheme::darkAnalyzer();
        refreshButtons();
    });
    connect(lightBtn, &QPushButton::clicked, this, [this]() {
        theme_ = WaveformColorTheme::classicWinSimLight();
        refreshButtons();
    });
    presetLay->addWidget(darkBtn);
    presetLay->addWidget(lightBtn);
    presetLay->addStretch();
    lay->addLayout(presetLay);

    refreshButtons();

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

void WaveformColorsDialog::refreshButtons() {
    for (const Swatch &sw : swatches_) {
        const QColor col = theme_.*(sw.member);
        sw.btn->setText(col.name().toUpper());
        const QColor fg = (col.lightness() > 140) ? Qt::black : Qt::white;
        sw.btn->setStyleSheet(
            QStringLiteral("background-color: %1; color: %2; border: 1px solid #64748b; padding: 3px;")
                .arg(col.name(), fg.name()));
    }
}

VectorAnnotationDialog::VectorAnnotationDialog(int vecIndex, const QString &comment,
                                               const QStringList &messages, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Vector #%1 Comment & $MSG").arg(vecIndex + 1));
    setMinimumWidth(440);

    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    commentEdit_ = new QLineEdit(comment, this);
    commentEdit_->setPlaceholderText(QStringLiteral("Inline /* ... */ comment on this vector"));

    msgEdit_ = new QPlainTextEdit(messages.join(QLatin1Char('\n')), this);
    msgEdit_->setPlaceholderText(QStringLiteral("One $MSG line per line (printed in .SO listing before this vector)"));
    msgEdit_->setMaximumHeight(110);

    form->addRow(QStringLiteral("Vector Comment:"), commentEdit_);
    form->addRow(QStringLiteral("$MSG Lines:"), msgEdit_);
    lay->addLayout(form);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

QString VectorAnnotationDialog::commentResult() const {
    QString c = commentEdit_->text().trimmed();
    c.replace(QStringLiteral("*/"), QStringLiteral("* /"));
    return c;
}

QStringList VectorAnnotationDialog::messagesResult() const {
    QStringList out;
    const QStringList lines = msgEdit_->toPlainText().split(QLatin1Char('\n'));
    for (const QString &l : lines) {
        if (!l.trimmed().isEmpty()) {
            out.append(l);
        }
    }
    return out;
}

SetLibraryDialog::SetLibraryDialog(const QString &currentLib, const QStringList &discoveredLibs,
                                   bool appendJedec, bool usePldName, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Simulator Library & Options"));
    setMinimumWidth(520);

    auto *lay = new QVBoxLayout(this);

    auto *libRow = new QHBoxLayout();
    libCombo_ = new QComboBox(this);
    libCombo_->setEditable(true);
    libCombo_->addItem(QStringLiteral("(Auto-detect via LIBCUPL / Share Directory)"), QString());
    for (const QString &p : discoveredLibs) {
        if (libCombo_->findText(p) < 0) libCombo_->addItem(p, p);
    }
    if (!currentLib.isEmpty()) {
        int idx = libCombo_->findText(currentLib);
        if (idx >= 0) libCombo_->setCurrentIndex(idx);
        else libCombo_->setEditText(currentLib);
    }

    auto *browseBtn = new QPushButton(QStringLiteral("Browse..."), this);
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        QString fn = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select CUPL Device Library (.dl)"),
            QString(), QStringLiteral("CUPL Device Libraries (*.dl *.DL);;All Files (*)"));
        if (!fn.isEmpty()) {
            libCombo_->setEditText(fn);
        }
    });
    libRow->addWidget(libCombo_, 1);
    libRow->addWidget(browseBtn);

    lay->addWidget(new QLabel(QStringLiteral("Device Library (.dl) passed via csim -u:"), this));
    lay->addLayout(libRow);

    auto *optBox = new QGroupBox(QStringLiteral("CSIM Output Options"), this);
    auto *optLay = new QVBoxLayout(optBox);
    jedecCheck_ = new QCheckBox(QStringLiteral("Append simulated test vectors (*V) to JEDEC file (-j)"), optBox);
    jedecCheck_->setChecked(appendJedec);
    pldNameCheck_ = new QCheckBox(QStringLiteral("Use PLD source filename for JEDEC output (-n)"), optBox);
    pldNameCheck_->setChecked(usePldName);
    optLay->addWidget(jedecCheck_);
    optLay->addWidget(pldNameCheck_);
    lay->addWidget(optBox);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

QString SetLibraryDialog::libraryPath() const {
    if (libCombo_->currentIndex() == 0 && libCombo_->currentText().startsWith(QStringLiteral("(Auto"))) {
        return QString();
    }
    return libCombo_->currentText().trimmed();
}

bool SetLibraryDialog::appendJedecFlag() const { return jedecCheck_->isChecked(); }
bool SetLibraryDialog::usePldNameFlag() const { return pldNameCheck_->isChecked(); }

FindDialog::FindDialog(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Find"));
    resize(440, 170);

    auto *grid = new QGridLayout(this);
    findEdit_ = new QLineEdit(this);
    targetCombo_ = new QComboBox(this);
    targetCombo_->addItem(QStringLiteral("Signal name"), static_cast<int>(Target::SignalName));
    targetCombo_->addItem(QStringLiteral("Vector number"), static_cast<int>(Target::VectorNumber));
    targetCombo_->addItem(QStringLiteral("Vector comment / $MSG"), static_cast<int>(Target::VectorText));
    scopeLabel_ = new QLabel(this);
    caseCheck_ = new QCheckBox(QStringLiteral("Match case"), this);
    wholeCheck_ = new QCheckBox(QStringLiteral("Whole words"), this);
    statusLabel_ = new QLabel(this);

    auto *btnNext = new QPushButton(QStringLiteral("Find Next"), this);
    auto *btnPrev = new QPushButton(QStringLiteral("Find Previous"), this);
    auto *btnClose = new QPushButton(QStringLiteral("Close"), this);
    btnNext->setDefault(true);

    grid->addWidget(new QLabel(QStringLiteral("Find what:"), this), 0, 0);
    grid->addWidget(findEdit_, 0, 1);
    grid->addWidget(btnNext, 0, 2);

    grid->addWidget(new QLabel(QStringLiteral("Look for:"), this), 1, 0);
    grid->addWidget(targetCombo_, 1, 1);
    grid->addWidget(btnPrev, 1, 2);

    auto *chkRow = new QHBoxLayout();
    chkRow->addWidget(caseCheck_);
    chkRow->addWidget(wholeCheck_);
    chkRow->addStretch();
    grid->addLayout(chkRow, 2, 0, 1, 2);
    grid->addWidget(btnClose, 2, 2);

    grid->addWidget(scopeLabel_, 3, 0, 1, 3);
    grid->addWidget(statusLabel_, 4, 0, 1, 3);

    connect(btnNext, &QPushButton::clicked, this, [this]() { emit findRequested(false); });
    connect(btnPrev, &QPushButton::clicked, this, [this]() { emit findRequested(true); });
    connect(btnClose, &QPushButton::clicked, this, &QDialog::close);
    connect(findEdit_, &QLineEdit::textChanged, statusLabel_, &QLabel::clear);
    connect(targetCombo_, &QComboBox::currentIndexChanged, statusLabel_, &QLabel::clear);
}

void FindDialog::focusFind(const QString &seedText) {
    if (!seedText.isEmpty()) {
        findEdit_->setText(seedText);
    }
    show();
    raise();
    activateWindow();
    findEdit_->setFocus();
    findEdit_->selectAll();
}

void FindDialog::setScope(const QString &scopeName, bool waveform) {
    targetCombo_->setEnabled(waveform);
    scopeLabel_->setText(QStringLiteral("Searching: %1").arg(scopeName));
    statusLabel_->clear();
}

void FindDialog::setStatus(const QString &message) {
    statusLabel_->setText(message);
}

QString FindDialog::text() const {
    return findEdit_->text();
}

void FindDialog::setText(const QString &text) {
    findEdit_->setText(text);
}

FindDialog::Target FindDialog::target() const {
    return static_cast<Target>(targetCombo_->currentData().toInt());
}

void FindDialog::setTarget(Target target) {
    targetCombo_->setCurrentIndex(targetCombo_->findData(static_cast<int>(target)));
}

bool FindDialog::caseSensitive() const {
    return caseCheck_->isChecked();
}

bool FindDialog::wholeWords() const {
    return wholeCheck_->isChecked();
}
