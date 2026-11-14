#include "compiler_options.h"
#include "tool_paths.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

const QString kCompilerGroup = QStringLiteral("compilerFlags/");
const QString kSimulatorGroup = QStringLiteral("simulatorFlags/");
const QString kVersionKey = QStringLiteral("version");
const int kSettingsVersion = 1;

const char *const kMinimizationNames[] = {
    "None", "Quick", "Quine-McCluskey", "Presto", "Espresso"
};

} // namespace

// The check boxes in the order of the dialog; buildFlagArgs passes the flags
// in this order.
const QList<CompilerOptions::Box> &CompilerOptions::boxes() {
    static const QList<Box> list = {
        {"secureDevice", "General", "Secure device (program the security fuse)", "-g",
         "Sets the security fuse in the JEDEC file (G1): a device programmed from it "
         "cannot be read back or verified.  The ATF150x fitters ignore it; use "
         "PROPERTY ATMEL {SECURITY = ON} for those.",
         &CompilerOptions::secureDevice, true, nullptr},
        {"deactivateUnusedOr", "General", "Deactivate unused OR terms (IFL devices)", "-d",
         "Switches off the OR-array connections of unused product terms.  Only "
         "IFL devices (82S153 and similar) have them.",
         &CompilerOptions::deactivateUnusedOr, true, nullptr},
        {"simulate", "General", "Simulate after compiling", "-s",
         "Runs the simulator on the design's .si file after a successful compile: "
         "writes .so and .wo and, with the JEDEC file on, adds the test vectors to "
         "the .jed.  The compile fails if there is no .si file.",
         &CompilerOptions::simulate, false, nullptr},
        {"oneHot", "General", "One-hot state machines", "-o",
         "The state numbers of SEQUENCE blocks are one-hot codes: each state is "
         "recognised by its own state bit.",
         &CompilerOptions::oneHot, false, nullptr},
        {"jedecNameIsPldName", "General", "JEDEC file named after the design file", "-n",
         "counter.pld gives counter.jed (and counter.hex) instead of the name in the "
         "Name field.",
         &CompilerOptions::jedecNameIsPldName, true, nullptr},

        {"bestPolarity", "Minimization", "Best polarity for each output", "-kb",
         "Chooses for each pin and node the polarity that needs fewer product terms; "
         "overrides DEMORGAN statements.",
         &CompilerOptions::bestPolarity, false, "demorgan"},
        {"demorgan", "Minimization", "DeMorgan all pins and nodes", "-kd",
         "Applies DeMorgan's theorem to every pin and node; overrides DEMORGAN "
         "statements.",
         &CompilerOptions::demorgan, false, "bestPolarity"},
        {"keepXor", "Minimization", "Keep XOR equations", "-kx",
         "XOR is not expanded to AND-OR; for the virtual device and for fitters "
         "with XOR gates.",
         &CompilerOptions::keepXor, false, nullptr},
        {"shareTerms", "Minimization", "Product-term sharing", "-ks",
         "Quine-McCluskey, Presto and Espresso minimize all outputs together so that "
         "they can share product terms.",
         &CompilerOptions::shareTerms, false, "noSharing"},
        {"noSharing", "Minimization", "Never share product terms", "-r",
         "Outputs never share a product term, not even the single AR and SP terms "
         "of a 22V10; designs that use those no longer fit.",
         &CompilerOptions::noSharing, false, "shareTerms"},

        {"jedec", "Download", "JEDEC file (.jed)", "-j",
         "The programming file.  For ATF150x and other fitter devices cupl also "
         "runs the fitter, which writes it.",
         &CompilerOptions::jedec, true, nullptr},
        {"hlFile", "Download", "HL file (Signetics IFL devices)", "-i",
         "Download file in HL format (.hl); other devices get a note in the listing.",
         &CompilerOptions::hlFile, true, nullptr},
        {"asciiHex", "Download", "ASCII-hex file (PROMs)", "-h",
         "Download file in ASCII-hex format (.hex); other devices get a note in the "
         "listing.",
         &CompilerOptions::asciiHex, true, nullptr},

        {"edif", "Output", "EDIF netlist (.edf)", "-t", "EDIF netlist of the design.",
         &CompilerOptions::edif, false, nullptr},
        {"palasm", "Output", "PALASM source (.pds)", "-c", "The design as PALASM source.",
         &CompilerOptions::palasm, false, nullptr},
        {"pla", "Output", "Berkeley PLA (.pla)", "-b",
         "The product terms in Berkeley PLA format.",
         &CompilerOptions::pla, false, nullptr},
        {"pdif", "Output", "PDIF netlist (.pdf)", "-p",
         "P-CAD PDIF netlist (a text file, not a PDF document).",
         &CompilerOptions::pdif, false, nullptr},
        {"expandedMacro", "Output", "Expanded macros (.mx)", "-e",
         "The design after $MACRO, $REPEAT and $INCLUDE were expanded.",
         &CompilerOptions::expandedMacro, false, nullptr},
        {"list", "Output", "Listing (.lst)", "-l",
         "The source with line numbers and the errors and warnings under their lines.",
         &CompilerOptions::list, false, nullptr},
        {"absolute", "Output", "Absolute file for the simulator (.abs)", "-a",
         "The compiled design for the simulator (.abs) and its readable form (.sim).",
         &CompilerOptions::absolute, false, nullptr},

        {"equations", "Doc File Options", "Equations", "-x",
         "Documentation file (.doc) with the minimized equations and the symbol table.",
         &CompilerOptions::equations, false, nullptr},
        {"fusePlot", "Doc File Options", "Fuse plot", "-f",
         "Adds the fuse plot and the chip diagram to the .doc file, which it writes "
         "even without Equations; not for devices placed by a fitter.",
         &CompilerOptions::fusePlot, true, nullptr},
    };
    return list;
}

namespace {

void writeCompilerOptions(QSettings &s, const CompilerOptions &o) {
    s.setValue(kCompilerGroup + kVersionKey, kSettingsVersion);
    s.setValue(kCompilerGroup + QStringLiteral("minimization"), o.minimization);
    for (const CompilerOptions::Box &b : CompilerOptions::boxes()) {
        s.setValue(kCompilerGroup + QLatin1String(b.key), o.*(b.member));
    }
}

void writeSimulatorOptions(QSettings &s, const SimulatorOptions &o) {
    s.setValue(kSimulatorGroup + kVersionKey, kSettingsVersion);
    s.setValue(kSimulatorGroup + QStringLiteral("writeListing"), o.writeListing);
    s.setValue(kSimulatorGroup + QStringLiteral("appendVectorsToJedec"), o.appendVectorsToJedec);
    s.setValue(kSimulatorGroup + QStringLiteral("usePldFilename"), o.usePldFilename);
    s.setValue(kSimulatorGroup + QStringLiteral("autoOpenLinSim"), o.autoOpenLinSim);
}

} // namespace

void CompilerOptions::migrateLegacySettings(QSettings &s) {
    s.beginGroup(QStringLiteral("compiler"));
    const bool hasLegacy = !s.childKeys().isEmpty();
    s.endGroup();
    if (!hasLegacy) {
        return;
    }
    auto old = [&s](const char *key, bool def) {
        return s.value(QStringLiteral("compiler/") + QLatin1String(key), def).toBool();
    };

    // Earlier versions labelled their check boxes after cupl's usage text,
    // which often does not say what the flag does.  A box is carried over
    // only where its label asked for what the flag did; "Gate-Level
    // Equations" (-g, the security fuse), "Append Fuse Plot" (-d), "Create
    // ALL Download Files" (-h), the DeMorgan mode that passed -kb,
    // "Deactivate unused global OR gates" (-z, rejected by cupl), "Warning
    // Messages OFF" (-w) and "Subtle Warnings OFF" (-q) are dropped and their
    // new boxes start at the default.  "JEDEC without Fuse Numbers" passed
    // -jx, which is -j and -x: it keeps the JEDEC file on.
    CompilerOptions o;
    o.minimization = qBound(0, s.value(QStringLiteral("compiler/minimizationLevel"), 1).toInt(), 4);
    o.demorgan = s.value(QStringLiteral("compiler/demorganMode"), 0).toInt() == 2;
    o.shareTerms = old("demorganShare", false);
    o.noSharing = old("noExpandMacro", false);
    o.oneHot = old("oneHotEncoding", false);
    o.jedec = old("jedec", true) || old("jedecNoFuseNums", false);
    o.absolute = old("absFile", true);
    o.pla = old("plaFile", false);
    o.palasm = old("centralFuseNums", false);
    o.expandedMacro = old("expandedMacro", false);
    o.hlFile = old("iflFile", false);
    o.list = old("listingFile", true);
    o.jedecNameIsPldName = old("useLogicFilename", false);
    o.pdif = old("pdifFile", false);
    o.edif = old("docTruthTable", false);
    o.equations = old("docExtendedListing", true);
    o.fusePlot = old("functionFile", false);   // "Chip Diagram / Function File", -f
    o.simulate = old("simulateAfterCompile", false) || old("waveformAfterCompile", false);

    s.remove(QStringLiteral("compiler"));
    writeCompilerOptions(s, o);
}

void CompilerOptions::loadFromSettings() {
    QSettings s;
    if (!s.contains(kCompilerGroup + kVersionKey)) {
        migrateLegacySettings(s);
    }
    const CompilerOptions defaults;
    minimization = qBound(0, s.value(kCompilerGroup + QStringLiteral("minimization"),
                                     defaults.minimization).toInt(), 4);
    for (const Box &b : boxes()) {
        this->*(b.member) = s.value(kCompilerGroup + QLatin1String(b.key), defaults.*(b.member)).toBool();
    }
}

void CompilerOptions::saveToSettings() const {
    QSettings s;
    writeCompilerOptions(s, *this);
}

QStringList CompilerOptions::buildFlagArgs(bool deviceIndependent) const {
    QStringList args;
    args << QStringLiteral("-m%1").arg(qBound(0, minimization, 4));
    for (const Box &b : boxes()) {
        if (deviceIndependent && b.deviceOnly) {
            continue;
        }
        bool on = this->*(b.member);
        // the virtual device always gets the files the simulator and the
        // reader need
        if (deviceIndependent && (b.member == &CompilerOptions::absolute ||
                                  b.member == &CompilerOptions::list ||
                                  b.member == &CompilerOptions::equations)) {
            on = true;
        }
        if (on) {
            args << QLatin1String(b.flag);
        }
    }
    return args;
}

void SimulatorOptions::migrateLegacySettings(QSettings &s) {
    s.beginGroup(QStringLiteral("simulator"));
    const bool hasLegacy = !s.childKeys().isEmpty();
    s.endGroup();
    if (!hasLegacy) {
        return;
    }
    // csim's -l and -w both write the .so and the .wo file, and -v changes
    // nothing (csim always prints to the console): the two file boxes become
    // one and "Verbose" is dropped.
    SimulatorOptions o;
    o.writeListing = s.value(QStringLiteral("simulator/listingFile"), true).toBool() ||
                     s.value(QStringLiteral("simulator/waveformFile"), true).toBool();
    o.appendVectorsToJedec = s.value(QStringLiteral("simulator/appendVectorsToJedec"), false).toBool();
    o.usePldFilename = s.value(QStringLiteral("simulator/usePldFilename"), false).toBool();
    o.autoOpenLinSim = s.value(QStringLiteral("simulator/autoOpenLinSim"), false).toBool();
    s.remove(QStringLiteral("simulator"));
    writeSimulatorOptions(s, o);
}

void SimulatorOptions::loadFromSettings() {
    QSettings s;
    if (!s.contains(kSimulatorGroup + kVersionKey)) {
        migrateLegacySettings(s);
    }
    const SimulatorOptions d;
    writeListing = s.value(kSimulatorGroup + QStringLiteral("writeListing"), d.writeListing).toBool();
    appendVectorsToJedec = s.value(kSimulatorGroup + QStringLiteral("appendVectorsToJedec"),
                                   d.appendVectorsToJedec).toBool();
    usePldFilename = s.value(kSimulatorGroup + QStringLiteral("usePldFilename"), d.usePldFilename).toBool();
    autoOpenLinSim = s.value(kSimulatorGroup + QStringLiteral("autoOpenLinSim"), d.autoOpenLinSim).toBool();
}

void SimulatorOptions::saveToSettings() const {
    QSettings s;
    writeSimulatorOptions(s, *this);
}

QStringList SimulatorOptions::buildFlagArgs(bool deviceIndependent) const {
    QStringList args;
    if (writeListing) args << QStringLiteral("-l");
    if (appendVectorsToJedec && !deviceIndependent) args << QStringLiteral("-j");
    if (usePldFilename) args << QStringLiteral("-n");
    return args;
}

// ---------------------------------------------------------------------------
// CompilerOptionsDialog
// ---------------------------------------------------------------------------
CompilerOptionsDialog::CompilerOptionsDialog(const CompilerOptions &initial, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Compiler Options"));

    auto *mainLayout = new QVBoxLayout(this);
    auto *columns = new QHBoxLayout();
    QVBoxLayout *column[2] = {new QVBoxLayout(), new QVBoxLayout()};

    // left: General, Minimization; right: Output, Download, Doc File Options
    struct GroupPlace {
        const char *name;
        int column;
    };
    const GroupPlace places[] = {
        {"General", 0}, {"Minimization", 0},
        {"Output", 1}, {"Download", 1}, {"Doc File Options", 1},
    };
    QHash<QString, QVBoxLayout *> groupLayouts;
    for (const GroupPlace &p : places) {
        auto *box = new QGroupBox(QLatin1String(p.name), this);
        auto *vbox = new QVBoxLayout(box);
        groupLayouts.insert(QLatin1String(p.name), vbox);
        column[p.column]->addWidget(box);
    }
    for (QVBoxLayout *c : column) {
        c->addStretch(1);
        columns->addLayout(c, 1);
    }

    QVBoxLayout *minLayout = groupLayouts.value(QStringLiteral("Minimization"));
    m_minGroup = new QButtonGroup(this);
    for (int level = 0; level <= 4; ++level) {
        auto *radio = new QRadioButton(QLatin1String(kMinimizationNames[level]), this);
        radio->setObjectName(QStringLiteral("minimization%1").arg(level));
        radio->setToolTip(QStringLiteral("-m%1").arg(level));
        m_minGroup->addButton(radio, level);
        minLayout->addWidget(radio);
    }
    minLayout->addSpacing(6);

    for (const CompilerOptions::Box &b : CompilerOptions::boxes()) {
        auto *chk = new QCheckBox(QLatin1String(b.label), this);
        chk->setObjectName(QLatin1String(b.key));
        chk->setToolTip(QStringLiteral("<p><tt>cupl %1</tt><br>%2</p>")
                            .arg(QLatin1String(b.flag), QString::fromLatin1(b.tip).toHtmlEscaped()));
        groupLayouts.value(QLatin1String(b.group))->addWidget(chk);
        m_boxes << chk;
    }
    // boxes whose flags undo each other: checking one clears the other
    for (int i = 0; i < m_boxes.size(); ++i) {
        const char *other = CompilerOptions::boxes().at(i).exclusiveWith;
        if (other == nullptr) {
            continue;
        }
        QCheckBox *otherBox = findChild<QCheckBox *>(QLatin1String(other));
        connect(m_boxes.at(i), &QCheckBox::toggled, this, [otherBox](bool on) {
            if (on && otherBox) {
                otherBox->setChecked(false);
            }
        });
    }
    mainLayout->addLayout(columns);

    auto *previewRow = new QHBoxLayout();
    previewRow->addWidget(new QLabel(QStringLiteral("Command line:"), this));
    m_previewEdit = new QLineEdit(this);
    m_previewEdit->setReadOnly(true);
    previewRow->addWidget(m_previewEdit, 1);
    mainLayout->addLayout(previewRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, &CompilerOptionsDialog::restoreDefaults);
    mainLayout->addWidget(buttons);

    connect(m_minGroup, &QButtonGroup::idToggled, this, &CompilerOptionsDialog::updatePreview);
    for (QCheckBox *cb : m_boxes) {
        connect(cb, &QCheckBox::toggled, this, &CompilerOptionsDialog::updatePreview);
    }

    applyToWidgets(initial);
}

void CompilerOptionsDialog::applyToWidgets(const CompilerOptions &opts) {
    if (QAbstractButton *radio = m_minGroup->button(qBound(0, opts.minimization, 4))) {
        radio->setChecked(true);
    }
    // set every box first without the exclusivity handlers, so that the
    // stored state is shown as it is
    for (int i = 0; i < m_boxes.size(); ++i) {
        const QSignalBlocker block(m_boxes.at(i));
        m_boxes.at(i)->setChecked(opts.*(CompilerOptions::boxes().at(i).member));
    }
    updatePreview();
}

CompilerOptions CompilerOptionsDialog::options() const {
    CompilerOptions o;
    o.minimization = qMax(0, m_minGroup->checkedId());
    for (int i = 0; i < m_boxes.size(); ++i) {
        o.*(CompilerOptions::boxes().at(i).member) = m_boxes.at(i)->isChecked();
    }
    return o;
}

void CompilerOptionsDialog::restoreDefaults() {
    applyToWidgets(CompilerOptions{});
}

void CompilerOptionsDialog::updatePreview() {
    m_previewEdit->setText(QStringLiteral("cupl ") +
                           options().buildFlagArgs(false).join(QLatin1Char(' ')) +
                           QStringLiteral(" -u <library.dl> [device] <design.pld>"));
}

// ---------------------------------------------------------------------------
// SimulatorOptionsDialog
// ---------------------------------------------------------------------------
SimulatorOptionsDialog::SimulatorOptionsDialog(const SimulatorOptions &initial, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Simulator Options"));

    auto *layout = new QVBoxLayout(this);
    auto *group = new QGroupBox(QStringLiteral("Simulator"), this);
    auto *vbox = new QVBoxLayout(group);

    auto makeBox = [&](QCheckBox *&chk, const char *name, const QString &label, const QString &tip) {
        chk = new QCheckBox(label, group);
        chk->setObjectName(QLatin1String(name));
        chk->setToolTip(tip);
        vbox->addWidget(chk);
    };
    makeBox(m_chkListing, "writeListing",
            QStringLiteral("Write the simulation listing and waveform file (.so, .wo)"),
            QStringLiteral("<p><tt>csim -l</tt><br>The listing shows every vector with the "
                           "simulated values; LinSim shows the waveform file.  Without it "
                           "the vectors appear only in the build log.</p>"));
    makeBox(m_chkAppendJed, "appendVectorsToJedec",
            QStringLiteral("Add the test vectors to the JEDEC file"),
            QStringLiteral("<p><tt>csim -j</tt><br>For device programmers that test the "
                           "programmed part; not for the virtual device.</p>"));
    makeBox(m_chkUsePldName, "usePldFilename",
            QStringLiteral("JEDEC file named after the design file"),
            QStringLiteral("<p><tt>csim -n</tt><br>Set it like the compiler option of the "
                           "same name, so that the vectors go into the JEDEC file the "
                           "compiler wrote.</p>"));
    vbox->addSpacing(6);
    makeBox(m_chkAutoLinSim, "autoOpenLinSim",
            QStringLiteral("Open LinSim after simulating"),
            QStringLiteral("Shows the waveform file in LinSim after a successful simulation."));
    layout->addWidget(group);

    auto *previewRow = new QHBoxLayout();
    previewRow->addWidget(new QLabel(QStringLiteral("Command line:"), this));
    m_previewEdit = new QLineEdit(this);
    m_previewEdit->setReadOnly(true);
    previewRow->addWidget(m_previewEdit, 1);
    layout->addLayout(previewRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, &SimulatorOptionsDialog::restoreDefaults);
    layout->addWidget(buttons);

    for (QCheckBox *cb : findChildren<QCheckBox *>()) {
        connect(cb, &QCheckBox::toggled, this, &SimulatorOptionsDialog::updatePreview);
    }

    applyToWidgets(initial);
}

void SimulatorOptionsDialog::applyToWidgets(const SimulatorOptions &opts) {
    m_chkListing->setChecked(opts.writeListing);
    m_chkAppendJed->setChecked(opts.appendVectorsToJedec);
    m_chkUsePldName->setChecked(opts.usePldFilename);
    m_chkAutoLinSim->setChecked(opts.autoOpenLinSim);
    updatePreview();
}

SimulatorOptions SimulatorOptionsDialog::options() const {
    SimulatorOptions o;
    o.writeListing = m_chkListing->isChecked();
    o.appendVectorsToJedec = m_chkAppendJed->isChecked();
    o.usePldFilename = m_chkUsePldName->isChecked();
    o.autoOpenLinSim = m_chkAutoLinSim->isChecked();
    return o;
}

void SimulatorOptionsDialog::restoreDefaults() {
    applyToWidgets(SimulatorOptions{});
}

void SimulatorOptionsDialog::updatePreview() {
    m_previewEdit->setText(QStringLiteral("csim ") +
                           options().buildFlagArgs(false).join(QLatin1Char(' ')) +
                           QStringLiteral(" -u <library.dl> [device] <design>"));
}

// ---------------------------------------------------------------------------
// EnvironmentPathsDialog
// ---------------------------------------------------------------------------
EnvironmentPathsDialog::EnvironmentPathsDialog(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Environment && Tool Paths"));
    resize(640, 340);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    auto *libRow = new QHBoxLayout();
    m_libEdit = new QLineEdit(ToolPaths::activeDeviceLibrary(), this);
    auto *libBtn = new QPushButton(QStringLiteral("Browse..."), this);
    connect(libBtn, &QPushButton::clicked, this, &EnvironmentPathsDialog::browseLibrary);
    libRow->addWidget(m_libEdit, 1);
    libRow->addWidget(libBtn);
    form->addRow(QStringLiteral("Device Library (.dl):"), libRow);

    auto *binRow = new QHBoxLayout();
    m_binDirEdit = new QLineEdit(ToolPaths::customBinDir(), this);
    m_binDirEdit->setPlaceholderText(QStringLiteral("(Auto-discover sibling binaries in <appDir>, build/bin)"));
    auto *binBtn = new QPushButton(QStringLiteral("Browse..."), this);
    connect(binBtn, &QPushButton::clicked, this, &EnvironmentPathsDialog::browseBinDir);
    binRow->addWidget(m_binDirEdit, 1);
    binRow->addWidget(binBtn);
    form->addRow(QStringLiteral("Custom Tool Bin Dir:"), binRow);

    layout->addLayout(form);

    auto *statusGroup = new QGroupBox(QStringLiteral("Resolved Binaries && Data Files"), this);
    auto *statusLayout = new QVBoxLayout(statusGroup);
    m_statusLabel = new QLabel(statusGroup);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel->setWordWrap(true);
    statusLayout->addWidget(m_statusLabel);
    layout->addWidget(statusGroup, 1);

    connect(m_binDirEdit, &QLineEdit::textChanged, this, &EnvironmentPathsDialog::refreshDetectedTools);
    connect(m_libEdit, &QLineEdit::textChanged, this, &EnvironmentPathsDialog::refreshDetectedTools);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    refreshDetectedTools();
}

QString EnvironmentPathsDialog::selectedLibraryPath() const {
    return m_libEdit->text().trimmed();
}

QString EnvironmentPathsDialog::customBinDir() const {
    return m_binDirEdit->text().trimmed();
}

void EnvironmentPathsDialog::browseLibrary() {
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select CUPL Device Library"),
        m_libEdit->text(), QStringLiteral("CUPL Device Libraries (*.dl *.DL);;All Files (*)"));
    if (!f.isEmpty()) {
        m_libEdit->setText(f);
    }
}

void EnvironmentPathsDialog::browseBinDir() {
    const QString d = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select Tool Binary Directory"), m_binDirEdit->text());
    if (!d.isEmpty()) {
        m_binDirEdit->setText(d);
    }
}

void EnvironmentPathsDialog::refreshDetectedTools() {
    const QString prevBin = ToolPaths::customBinDir();
    ToolPaths::setCustomBinDir(m_binDirEdit->text());

    auto fmt = [](const QString &name, const QString &path) {
        return QStringLiteral("<b>%1</b>: %2")
            .arg(name, path.isEmpty() ? QStringLiteral("<span style='color:#c93b2b;'>Not found</span>")
                                      : path.toHtmlEscaped());
    };

    QStringList lines;
    lines << fmt(QStringLiteral("cupl"), ToolPaths::findExecutable(QStringLiteral("cupl")));
    lines << fmt(QStringLiteral("csim"), ToolPaths::findExecutable(QStringLiteral("csim")));
    lines << fmt(QStringLiteral("cbld"), ToolPaths::findExecutable(QStringLiteral("cbld")));
    lines << fmt(QStringLiteral("backpin"), ToolPaths::findExecutable(QStringLiteral("backpin")));
    lines << fmt(QStringLiteral("linsim"), ToolPaths::findExecutable(QStringLiteral("linsim")));
    lines << fmt(QStringLiteral("Atmel.cat"), ToolPaths::findAtmelCat());
    lines << fmt(QStringLiteral("CuplErr.txt"), ToolPaths::findCuplErrTxt());
    m_statusLabel->setText(lines.join(QStringLiteral("<br/>")));

    ToolPaths::setCustomBinDir(prevBin);
}
