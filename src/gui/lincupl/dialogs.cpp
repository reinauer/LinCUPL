#include "dialogs.h"
#include "code_editor.h"
#include "../common/cupl_highlighter.h"
#include "tool_paths.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString stripCuplComments(const QString &src) {
    QString out;
    out.reserve(src.size());
    int i = 0;
    while (i < src.size()) {
        if (i + 1 < src.size() && src.at(i) == QLatin1Char('/') && src.at(i + 1) == QLatin1Char('*')) {
            const int end = src.indexOf(QStringLiteral("*/"), i + 2);
            if (end < 0) {
                break;
            }
            // Preserve newlines so line structure stays intact
            for (int k = i; k < end + 2; ++k) {
                if (src.at(k) == QLatin1Char('\n')) {
                    out.append(QLatin1Char('\n'));
                }
            }
            i = end + 2;
        } else {
            out.append(src.at(i));
            ++i;
        }
    }
    return out;
}

} // namespace

// ===========================================================================
// 1. DeviceBrowserDialog
// ===========================================================================
DeviceBrowserDialog::DeviceBrowserDialog(DeviceLibrary *library,
                                         const QString &initialMnemonic,
                                         bool hasOpenPld,
                                         QWidget *parent)
    : QDialog(parent), m_library(library), m_selectedMnemonic(initialMnemonic.trimmed().toLower()) {
    setWindowTitle(QStringLiteral("Device Selection"));
    resize(920, 580);

    auto *root = new QVBoxLayout(this);

    // Top library chooser bar
    auto *libBar = new QHBoxLayout();
    libBar->addWidget(new QLabel(QStringLiteral("Device Library (.dl):"), this));
    m_libCombo = new QComboBox(this);
    libBar->addWidget(m_libCombo, 1);
    auto *browseBtn = new QPushButton(QStringLiteral("Browse .dl..."), this);
    connect(browseBtn, &QPushButton::clicked, this, &DeviceBrowserDialog::browseCustomLibrary);
    libBar->addWidget(browseBtn);
    root->addLayout(libBar);

    m_libSummaryLabel = new QLabel(this);
    m_libSummaryLabel->setStyleSheet(QStringLiteral("color: #666; font-size: 11px;"));
    root->addWidget(m_libSummaryLabel);

    // Filter bar
    auto *filterBar = new QHBoxLayout();
    filterBar->addWidget(new QLabel(QStringLiteral("Package:"), this));
    m_pkgCombo = new QComboBox(this);
    m_pkgCombo->addItems({
        QStringLiteral("All Packages (DIP + LCC/PLCC)"),
        QStringLiteral("DIP / Standard"),
        QStringLiteral("LCC / PLCC")
    });
    filterBar->addWidget(m_pkgCombo);

    filterBar->addSpacing(12);
    filterBar->addWidget(new QLabel(QStringLiteral("Search:"), this));
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setPlaceholderText(QStringLiteral("Filter by mnemonic (e.g. g16v8, g22v10, v750, f1502), pins, or extension..."));
    filterBar->addWidget(m_searchEdit, 1);
    root->addLayout(filterBar);

    // 3-pane splitter: Categories | Device Table | Device Info & cbld details
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    m_catList = new QListWidget(splitter);
    m_catList->setMinimumWidth(150);

    m_deviceTree = new QTreeWidget(splitter);
    m_deviceTree->setRootIsDecorated(false);
    m_deviceTree->setAlternatingRowColors(true);
    m_deviceTree->setSortingEnabled(true);
    m_deviceTree->setHeaderLabels({
        QStringLiteral("Mnemonic"),
        QStringLiteral("Category"),
        QStringLiteral("Package"),
        QStringLiteral("Pins"),
        QStringLiteral("P-Terms"),
        QStringLiteral("Fuses"),
        QStringLiteral("Rev"),
        QStringLiteral("Lib ID")
    });
    m_deviceTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    auto *rightPane = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    m_detailsView = new QTextBrowser(rightPane);
    auto *cbldBtn = new QPushButton(QStringLiteral("Inspect with cbld (-l / -e)"), rightPane);
    connect(cbldBtn, &QPushButton::clicked, this, &DeviceBrowserDialog::runCbldInspect);
    rightLayout->addWidget(m_detailsView, 1);
    rightLayout->addWidget(cbldBtn);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 2);
    root->addWidget(splitter, 1);

    // Bottom target update options
    auto *optRow = new QHBoxLayout();
    m_chkUpdatePld = new QCheckBox(QStringLiteral("Update 'Device <mnemonic>;' in active .PLD file"), this);
    m_chkUpdatePld->setChecked(hasOpenPld);
    m_chkUpdatePld->setEnabled(hasOpenPld);
    m_chkOverride = new QCheckBox(QStringLiteral("Set active compiler device override"), this);
    m_chkOverride->setChecked(!hasOpenPld);
    optRow->addWidget(m_chkUpdatePld);
    optRow->addWidget(m_chkOverride);
    optRow->addStretch();
    root->addLayout(optRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_selectedMnemonic.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Select a Device"),
                                 QStringLiteral("Please select a target PLD device from the list."));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    populateLibraryCombo();

    connect(m_libCombo, &QComboBox::currentIndexChanged, this, &DeviceBrowserDialog::onLibraryComboChanged);
    connect(m_catList, &QListWidget::currentRowChanged, this, [this](int) { refreshFilter(); });
    connect(m_pkgCombo, &QComboBox::currentIndexChanged, this, [this](int) { refreshFilter(); });
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &) { refreshFilter(); });
    connect(m_deviceTree, &QTreeWidget::itemSelectionChanged, this, &DeviceBrowserDialog::onDeviceSelectionChanged);
    connect(m_deviceTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *, int) {
        if (!m_selectedMnemonic.isEmpty()) {
            accept();
        }
    });

    reloadCategoriesAndFilter();
}

void DeviceBrowserDialog::populateLibraryCombo() {
    m_libCombo->clear();
    QStringList libs = ToolPaths::findDeviceLibraries();
    const QString cur = m_library ? m_library->libraryPath() : QString();
    if (!cur.isEmpty() && !libs.contains(cur)) {
        libs.prepend(cur);
    }

    int activeIdx = 0;
    for (int i = 0; i < libs.size(); ++i) {
        const QString &p = libs.at(i);
        m_libCombo->addItem(QStringLiteral("%1  (%2)").arg(QFileInfo(p).fileName(), p), p);
        if (!cur.isEmpty() && QFileInfo(p).canonicalFilePath() == QFileInfo(cur).canonicalFilePath()) {
            activeIdx = i;
        }
    }
    if (m_libCombo->count() > 0) {
        m_libCombo->setCurrentIndex(activeIdx);
    }
}

QString DeviceBrowserDialog::selectedLibraryPath() const {
    return m_libCombo->currentData().toString();
}

bool DeviceBrowserDialog::shouldUpdatePldHeader() const {
    return m_chkUpdatePld->isEnabled() && m_chkUpdatePld->isChecked();
}

bool DeviceBrowserDialog::shouldSetCompilerOverride() const {
    return m_chkOverride->isChecked();
}

void DeviceBrowserDialog::onLibraryComboChanged(int index) {
    if (index < 0 || !m_library) {
        return;
    }
    const QString path = m_libCombo->itemData(index).toString();
    if (path.isEmpty()) {
        return;
    }
    QString err;
    if (!m_library->loadLibrary(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("Library Load Error"), err);
        return;
    }
    const QString cat = ToolPaths::findAtmelCat();
    if (!cat.isEmpty()) {
        m_library->loadCategories(cat);
    }
    reloadCategoriesAndFilter();
}

void DeviceBrowserDialog::browseCustomLibrary() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open CUPL .dl Device Library"),
        selectedLibraryPath(),
        QStringLiteral("CUPL Device Library (*.dl *.DL);;All Files (*)"));
    if (path.isEmpty()) {
        return;
    }
    const int existing = m_libCombo->findData(path);
    if (existing >= 0) {
        m_libCombo->setCurrentIndex(existing);
    } else {
        m_libCombo->addItem(QStringLiteral("%1  (%2)").arg(QFileInfo(path).fileName(), path), path);
        m_libCombo->setCurrentIndex(m_libCombo->count() - 1);
    }
}

void DeviceBrowserDialog::reloadCategoriesAndFilter() {
    if (!m_library) {
        return;
    }
    m_libSummaryLabel->setText(
        QStringLiteral("Library: %1 | Rev: %2 | Access Key: %3 | Devices: %4 | Categories: %5")
            .arg(m_library->libraryName(),
                 m_library->libraryRevision(),
                 QString::number(m_library->accessKey()),
                 QString::number(m_library->devices().size()),
                 m_library->categoryFilePath().isEmpty()
                     ? QStringLiteral("None")
                     : QFileInfo(m_library->categoryFilePath()).fileName()));

    m_catList->blockSignals(true);
    m_catList->clear();
    for (const QString &cat : m_library->categories()) {
        m_catList->addItem(cat);
    }
    if (m_catList->count() > 0) {
        m_catList->setCurrentRow(0);
    }
    m_catList->blockSignals(false);
    refreshFilter();
}

void DeviceBrowserDialog::refreshFilter() {
    if (!m_library) {
        return;
    }
    const QString cat = m_catList->currentItem() ? m_catList->currentItem()->text() : QStringLiteral("All Devices");
    DeviceLibrary::PackageFilter pf = DeviceLibrary::PackageFilter::All;
    if (m_pkgCombo->currentIndex() == 1) pf = DeviceLibrary::PackageFilter::DipStandard;
    if (m_pkgCombo->currentIndex() == 2) pf = DeviceLibrary::PackageFilter::LccPlcc;

    const QList<DeviceInfo> filtered = m_library->filterDevices(cat, pf, m_searchEdit->text());

    m_deviceTree->setSortingEnabled(false);
    m_deviceTree->clear();

    QTreeWidgetItem *itemToSelect = nullptr;
    for (const DeviceInfo &dev : filtered) {
        auto *item = new QTreeWidgetItem();
        item->setText(0, dev.mnemonic);
        item->setText(1, dev.category);
        item->setText(2, dev.packageGroup);
        item->setData(3, Qt::DisplayRole, dev.pins);
        item->setData(4, Qt::DisplayRole, static_cast<int>(dev.productTerms));
        item->setData(5, Qt::DisplayRole, static_cast<qulonglong>(dev.fuses));
        item->setText(6, QStringLiteral("%1").arg(dev.revision, 2, 10, QLatin1Char('0')));
        item->setData(7, Qt::DisplayRole, dev.directoryIndex);
        item->setData(0, Qt::UserRole, dev.mnemonic);
        m_deviceTree->addTopLevelItem(item);

        if (!m_selectedMnemonic.isEmpty() && dev.mnemonic == m_selectedMnemonic) {
            itemToSelect = item;
        }
    }
    m_deviceTree->setSortingEnabled(true);

    if (!itemToSelect && m_deviceTree->topLevelItemCount() > 0) {
        itemToSelect = m_deviceTree->topLevelItem(0);
    }
    if (itemToSelect) {
        m_deviceTree->setCurrentItem(itemToSelect);
        m_deviceTree->scrollToItem(itemToSelect);
    } else {
        m_detailsView->clear();
    }
}

void DeviceBrowserDialog::onDeviceSelectionChanged() {
    const QList<QTreeWidgetItem *> sel = m_deviceTree->selectedItems();
    if (sel.isEmpty() || !m_library) {
        return;
    }
    const QString mnem = sel.first()->data(0, Qt::UserRole).toString();
    const DeviceInfo dev = m_library->findDevice(mnem);
    if (!dev.isValid()) {
        return;
    }
    m_selectedMnemonic = dev.mnemonic;

    const QString extHtml = dev.extensions.isEmpty()
                                ? QStringLiteral("<i>None (Combinational / Virtual)</i>")
                                : QStringLiteral("<code>.%1</code>")
                                      .arg(dev.extensions.join(QStringLiteral("</code>, <code>.")));

    m_detailsView->setHtml(QStringLiteral(
        "<h3 style='margin-bottom:4px;'>%1</h3>"
        "<table cellspacing='3' cellpadding='2'>"
        "<tr><td><b>Category:</b></td><td>%2</td></tr>"
        "<tr><td><b>Package Group:</b></td><td>%3</td></tr>"
        "<tr><td><b>Pin Count:</b></td><td>%4</td></tr>"
        "<tr><td><b>Product Terms:</b></td><td>%5</td></tr>"
        "<tr><td><b>Fuse Count:</b></td><td>%6</td></tr>"
        "<tr><td><b>Device Revision:</b></td><td>%7</td></tr>"
        "<tr><td><b>Library ID (Slot):</b></td><td>#%8 (offset 0x%9, %10 bytes)</td></tr>"
        "<tr><td><b>Fitter Type:</b></td><td>%11</td></tr>"
        "</table>"
        "<p><b>Supported Extensions:</b><br/>%12</p>"
        "<p style='color:#666;font-size:11px;'>CUPL Header Syntax:<br/><code>Device %1;</code></p>")
        .arg(dev.mnemonic.toHtmlEscaped(),
             dev.category.toHtmlEscaped(),
             dev.packageGroup.toHtmlEscaped(),
             QString::number(dev.pins),
             QString::number(dev.productTerms),
             QString::number(dev.fuses),
             QStringLiteral("%1").arg(dev.revision, 2, 10, QLatin1Char('0')),
             QString::number(dev.directoryIndex),
             QString::number(dev.recordOffset, 16),
             QString::number(dev.recordSize),
             QString::number(dev.fitterType),
             extHtml));
}

void DeviceBrowserDialog::runCbldInspect() {
    if (m_selectedMnemonic.isEmpty() || !m_library) {
        return;
    }
    const QString cbldExe = ToolPaths::findExecutable(QStringLiteral("cbld"));
    if (cbldExe.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("cbld Not Found"),
                                 QStringLiteral("Could not find sibling 'cbld' executable. Direct .dl binary info is already shown above."));
        return;
    }

    auto runCmd = [&](const QString &flag) -> QString {
        QProcess proc;
        proc.start(cbldExe, {flag, QStringLiteral("-u"), m_library->libraryPath(), m_selectedMnemonic});
        proc.waitForFinished(3000);
        return ErrorCatalog::normalizeConsoleText(QString::fromLatin1(proc.readAllStandardOutput() + proc.readAllStandardError()));
    };

    const QString longOut = runCmd(QStringLiteral("-l"));
    const QString extOut = runCmd(QStringLiteral("-e"));
    m_detailsView->append(QStringLiteral("<hr/><b>cbld -l / -e Output:</b><pre>%1\n%2</pre>")
                              .arg(longOut.toHtmlEscaped(), extOut.toHtmlEscaped()));
}

// ===========================================================================
// 2. NewDesignDialog (frmNewDesign)
// ===========================================================================
NewDesignDialog::NewDesignDialog(DeviceLibrary *library,
                                 const QString &defaultDir,
                                 QWidget *parent)
    : QDialog(parent), m_library(library), m_defaultDir(defaultDir) {
    setWindowTitle(QStringLiteral("New CUPL Design / Project (PLD Wizard)"));
    resize(860, 620);

    auto *root = new QVBoxLayout(this);
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    auto *leftWidget = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 6, 0);

    // Header group
    auto *hdrGroup = new QGroupBox(QStringLiteral("PLD Header Information"), leftWidget);
    auto *hdrGrid = new QGridLayout(hdrGroup);

    m_nameEdit = new QLineEdit(QStringLiteral("NewDesign"), hdrGroup);
    m_partnoEdit = new QLineEdit(QStringLiteral("00"), hdrGroup);
    m_dateEdit = new QLineEdit(QDate::currentDate().toString(QStringLiteral("MM/dd/yyyy")), hdrGroup);
    m_revEdit = new QLineEdit(QStringLiteral("01"), hdrGroup);
    m_designerEdit = new QLineEdit(qEnvironmentVariable("USER", QStringLiteral("Engineer")), hdrGroup);
    m_companyEdit = new QLineEdit(QStringLiteral("LinCUPL"), hdrGroup);
    m_assemblyEdit = new QLineEdit(QStringLiteral("None"), hdrGroup);
    m_locationEdit = new QLineEdit(QStringLiteral("None"), hdrGroup);
    m_deviceEdit = new QLineEdit(QStringLiteral("g16v8a"), hdrGroup);
    auto *devBtn = new QPushButton(QStringLiteral("Devices..."), hdrGroup);
    connect(devBtn, &QPushButton::clicked, this, &NewDesignDialog::browseDevice);

    hdrGrid->addWidget(new QLabel(QStringLiteral("Name:"), hdrGroup), 0, 0);
    hdrGrid->addWidget(m_nameEdit, 0, 1);
    hdrGrid->addWidget(new QLabel(QStringLiteral("Partno:"), hdrGroup), 0, 2);
    hdrGrid->addWidget(m_partnoEdit, 0, 3);

    hdrGrid->addWidget(new QLabel(QStringLiteral("Date:"), hdrGroup), 1, 0);
    hdrGrid->addWidget(m_dateEdit, 1, 1);
    hdrGrid->addWidget(new QLabel(QStringLiteral("Revision:"), hdrGroup), 1, 2);
    hdrGrid->addWidget(m_revEdit, 1, 3);

    hdrGrid->addWidget(new QLabel(QStringLiteral("Designer:"), hdrGroup), 2, 0);
    hdrGrid->addWidget(m_designerEdit, 2, 1);
    hdrGrid->addWidget(new QLabel(QStringLiteral("Company:"), hdrGroup), 2, 2);
    hdrGrid->addWidget(m_companyEdit, 2, 3);

    hdrGrid->addWidget(new QLabel(QStringLiteral("Assembly:"), hdrGroup), 3, 0);
    hdrGrid->addWidget(m_assemblyEdit, 3, 1);
    hdrGrid->addWidget(new QLabel(QStringLiteral("Location:"), hdrGroup), 3, 2);
    hdrGrid->addWidget(m_locationEdit, 3, 3);

    auto *devRow = new QHBoxLayout();
    devRow->addWidget(m_deviceEdit, 1);
    devRow->addWidget(devBtn);
    hdrGrid->addWidget(new QLabel(QStringLiteral("Device:"), hdrGroup), 4, 0);
    hdrGrid->addLayout(devRow, 4, 1, 1, 3);

    auto *fileRow = new QHBoxLayout();
    m_filePathEdit = new QLineEdit(hdrGroup);
    m_filePathEdit->setPlaceholderText(QStringLiteral("(Optional) Choose path to save .PLD immediately, or leave empty for new tab"));
    auto *fileBtn = new QPushButton(QStringLiteral("Save As..."), hdrGroup);
    connect(fileBtn, &QPushButton::clicked, this, &NewDesignDialog::browseSavePath);
    fileRow->addWidget(m_filePathEdit, 1);
    fileRow->addWidget(fileBtn);
    hdrGrid->addWidget(new QLabel(QStringLiteral("File:"), hdrGroup), 5, 0);
    hdrGrid->addLayout(fileRow, 5, 1, 1, 3);

    leftLayout->addWidget(hdrGroup);

    // Pin & Pinnode counts + table
    auto *pinGroup = new QGroupBox(QStringLiteral("Input Pins, Output Pins && Pinnodes"), leftWidget);
    auto *pinVBox = new QVBoxLayout(pinGroup);
    auto *cntRow = new QHBoxLayout();
    m_inCountSpin = new QSpinBox(pinGroup);
    m_inCountSpin->setRange(0, 64);
    m_inCountSpin->setValue(2);
    m_outCountSpin = new QSpinBox(pinGroup);
    m_outCountSpin->setRange(0, 64);
    m_outCountSpin->setValue(1);
    m_nodeCountSpin = new QSpinBox(pinGroup);
    m_nodeCountSpin->setRange(0, 64);
    m_nodeCountSpin->setValue(0);

    cntRow->addWidget(new QLabel(QStringLiteral("Input Pins:"), pinGroup));
    cntRow->addWidget(m_inCountSpin);
    cntRow->addSpacing(12);
    cntRow->addWidget(new QLabel(QStringLiteral("Output Pins:"), pinGroup));
    cntRow->addWidget(m_outCountSpin);
    cntRow->addSpacing(12);
    cntRow->addWidget(new QLabel(QStringLiteral("Pinnodes:"), pinGroup));
    cntRow->addWidget(m_nodeCountSpin);
    cntRow->addStretch();
    pinVBox->addLayout(cntRow);

    m_pinTable = new QTableWidget(0, 5, pinGroup);
    m_pinTable->setHorizontalHeaderLabels({
        QStringLiteral("Type"),
        QStringLiteral("Pin/Node #"),
        QStringLiteral("Signal Name"),
        QStringLiteral("Active Low (!)"),
        QStringLiteral("Description")
    });
    m_pinTable->horizontalHeader()->setStretchLastSection(true);
    pinVBox->addWidget(m_pinTable, 1);
    leftLayout->addWidget(pinGroup, 1);

    // Right preview pane
    auto *rightWidget = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(6, 0, 0, 0);
    rightLayout->addWidget(new QLabel(QStringLiteral("Generated .PLD Template Preview:"), rightWidget));
    m_previewEdit = new QPlainTextEdit(rightWidget);
    m_previewEdit->setReadOnly(true);
    m_previewEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    new CuplHighlighter(m_previewEdit->document());
    rightLayout->addWidget(m_previewEdit, 1);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    for (QLineEdit *le : {m_nameEdit, m_partnoEdit, m_dateEdit, m_revEdit,
                          m_designerEdit, m_companyEdit, m_assemblyEdit,
                          m_locationEdit, m_deviceEdit}) {
        connect(le, &QLineEdit::textChanged, this, &NewDesignDialog::updatePreview);
    }
    connect(m_inCountSpin, &QSpinBox::valueChanged, this, &NewDesignDialog::syncPinRows);
    connect(m_outCountSpin, &QSpinBox::valueChanged, this, &NewDesignDialog::syncPinRows);
    connect(m_nodeCountSpin, &QSpinBox::valueChanged, this, &NewDesignDialog::syncPinRows);
    connect(m_pinTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *) { updatePreview(); });

    syncPinRows();
}

void NewDesignDialog::browseDevice() {
    DeviceBrowserDialog dlg(m_library, m_deviceEdit->text(), false, this);
    if (dlg.exec() == QDialog::Accepted && !dlg.selectedMnemonic().isEmpty()) {
        m_deviceEdit->setText(dlg.selectedMnemonic());
    }
}

void NewDesignDialog::browseSavePath() {
    const QString suggested = QDir(m_defaultDir).filePath(m_nameEdit->text().trimmed() + QStringLiteral(".pld"));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save New PLD File"), suggested,
        QStringLiteral("CUPL Source Files (*.pld *.PLD);;All Files (*)"));
    if (!path.isEmpty()) {
        m_filePathEdit->setText(path);
        const QString stem = QFileInfo(path).completeBaseName();
        if (!stem.isEmpty()) {
            m_nameEdit->setText(stem);
        }
    }
}

void NewDesignDialog::syncPinRows() {
    const NewDesignSpec prev = spec();

    const int nIn = m_inCountSpin->value();
    const int nOut = m_outCountSpin->value();
    const int nNode = m_nodeCountSpin->value();
    const int total = nIn + nOut + nNode;

    m_pinTable->blockSignals(true);
    m_pinTable->setRowCount(total);

    int row = 0;
    auto populateGroup = [&](const QString &kind, const QList<NewPinEntry> &oldList,
                             int count, int defaultStartPin, const QString &prefix) {
        for (int i = 0; i < count; ++i, ++row) {
            const NewPinEntry existing = (i < oldList.size())
                                             ? oldList.at(i)
                                             : NewPinEntry{
                                                   defaultStartPin > 0 ? QString::number(defaultStartPin + i) : QString(),
                                                   QStringLiteral("%1%2").arg(prefix).arg(i),
                                                   false,
                                                   QString()};

            auto *typeItem = new QTableWidgetItem(kind);
            typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
            m_pinTable->setItem(row, 0, typeItem);
            m_pinTable->setItem(row, 1, new QTableWidgetItem(existing.pinNumber));
            m_pinTable->setItem(row, 2, new QTableWidgetItem(existing.name));

            auto *lowItem = new QTableWidgetItem();
            lowItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            lowItem->setCheckState(existing.activeLow ? Qt::Checked : Qt::Unchecked);
            m_pinTable->setItem(row, 3, lowItem);

            m_pinTable->setItem(row, 4, new QTableWidgetItem(existing.comment));
        }
    };

    populateGroup(QStringLiteral("Input"), prev.inputs, nIn, 1, QStringLiteral("in"));
    populateGroup(QStringLiteral("Output"), prev.outputs, nOut, 12, QStringLiteral("out"));
    populateGroup(QStringLiteral("Pinnode"), prev.pinnodes, nNode, 0, QStringLiteral("node"));

    m_pinTable->blockSignals(false);
    updatePreview();
}

NewDesignSpec NewDesignDialog::spec() const {
    NewDesignSpec s;
    s.name = m_nameEdit->text().trimmed().isEmpty() ? QStringLiteral("NewDesign") : m_nameEdit->text().trimmed();
    s.partno = m_partnoEdit->text().trimmed().isEmpty() ? QStringLiteral("00") : m_partnoEdit->text().trimmed();
    s.date = m_dateEdit->text().trimmed().isEmpty()
                 ? QDate::currentDate().toString(QStringLiteral("MM/dd/yyyy"))
                 : m_dateEdit->text().trimmed();
    s.revision = m_revEdit->text().trimmed().isEmpty() ? QStringLiteral("01") : m_revEdit->text().trimmed();
    s.designer = m_designerEdit->text().trimmed().isEmpty() ? QStringLiteral("Engineer") : m_designerEdit->text().trimmed();
    s.company = m_companyEdit->text().trimmed().isEmpty() ? QStringLiteral("LinCUPL") : m_companyEdit->text().trimmed();
    s.assembly = m_assemblyEdit->text().trimmed().isEmpty() ? QStringLiteral("None") : m_assemblyEdit->text().trimmed();
    s.location = m_locationEdit->text().trimmed().isEmpty() ? QStringLiteral("None") : m_locationEdit->text().trimmed();
    s.device = m_deviceEdit->text().trimmed().isEmpty() ? QStringLiteral("g16v8a") : m_deviceEdit->text().trimmed();
    s.saveFilePath = m_filePathEdit->text().trimmed();

    for (int r = 0; r < m_pinTable->rowCount(); ++r) {
        const QString kind = m_pinTable->item(r, 0) ? m_pinTable->item(r, 0)->text() : QString();
        NewPinEntry e;
        e.pinNumber = m_pinTable->item(r, 1) ? m_pinTable->item(r, 1)->text().trimmed() : QString();
        e.name = m_pinTable->item(r, 2) ? m_pinTable->item(r, 2)->text().trimmed() : QString();
        e.activeLow = m_pinTable->item(r, 3) && m_pinTable->item(r, 3)->checkState() == Qt::Checked;
        e.comment = m_pinTable->item(r, 4) ? m_pinTable->item(r, 4)->text().trimmed() : QString();

        if (kind == QStringLiteral("Input")) s.inputs.append(e);
        else if (kind == QStringLiteral("Output")) s.outputs.append(e);
        else if (kind == QStringLiteral("Pinnode")) s.pinnodes.append(e);
    }
    return s;
}

QString NewDesignDialog::generatePldTemplate(const NewDesignSpec &spec) {
    QStringList lines;
    lines << QStringLiteral("Name     %1 ;").arg(spec.name);
    lines << QStringLiteral("PartNo   %1 ;").arg(spec.partno);
    lines << QStringLiteral("Date     %1 ;").arg(spec.date.isEmpty()
                                                     ? QDate::currentDate().toString(QStringLiteral("MM/dd/yyyy"))
                                                     : spec.date);
    lines << QStringLiteral("Revision %1 ;").arg(spec.revision);
    lines << QStringLiteral("Designer %1 ;").arg(spec.designer);
    lines << QStringLiteral("Company  %1 ;").arg(spec.company);
    lines << QStringLiteral("Assembly %1 ;").arg(spec.assembly);
    lines << QStringLiteral("Location %1 ;").arg(spec.location);
    lines << QStringLiteral("Device   %1 ;").arg(spec.device);
    lines << QString();

    auto formatDecl = [](const QString &kw, const NewPinEntry &p, const QString &fallbackName) {
        const QString sig = (p.activeLow ? QStringLiteral("!") : QString()) +
                            (p.name.isEmpty() ? fallbackName : p.name);
        const QString lhs = p.pinNumber.isEmpty()
                                ? kw
                                : QStringLiteral("%1 %2").arg(kw, p.pinNumber);
        QString line = QStringLiteral("%1 = %2 ;").arg(lhs.leftJustified(12), sig);
        if (!p.comment.isEmpty()) {
            line += QStringLiteral(" /* %1 */").arg(p.comment);
        }
        return line;
    };

    lines << QStringLiteral("/* *************** INPUT PINS *********************/");
    for (int i = 0; i < spec.inputs.size(); ++i) {
        lines << formatDecl(QStringLiteral("PIN"), spec.inputs.at(i), QStringLiteral("in%1").arg(i));
    }
    lines << QString();

    lines << QStringLiteral("/* *************** OUTPUT PINS ********************/");
    for (int i = 0; i < spec.outputs.size(); ++i) {
        lines << formatDecl(QStringLiteral("PIN"), spec.outputs.at(i), QStringLiteral("out%1").arg(i));
    }
    lines << QString();

    if (!spec.pinnodes.isEmpty()) {
        lines << QStringLiteral("/* *************** PINNODES ***********************/");
        for (int i = 0; i < spec.pinnodes.size(); ++i) {
            lines << formatDecl(QStringLiteral("PINNODE"), spec.pinnodes.at(i), QStringLiteral("node%1").arg(i));
        }
        lines << QString();
    }

    lines << QStringLiteral("/* *************** LOGIC EQUATIONS ****************/");
    if (!spec.outputs.isEmpty()) {
        QString rhs = QStringLiteral("'b'0");
        if (!spec.inputs.isEmpty()) {
            QStringList inNames;
            for (const NewPinEntry &in : spec.inputs) {
                if (!in.name.isEmpty()) {
                    inNames.append(in.name);
                }
            }
            if (!inNames.isEmpty()) {
                rhs = inNames.join(QStringLiteral(" & "));
            }
        }
        for (int i = 0; i < spec.outputs.size(); ++i) {
            const QString outName = spec.outputs.at(i).name.isEmpty()
                                        ? QStringLiteral("out%1").arg(i)
                                        : spec.outputs.at(i).name;
            lines << QStringLiteral("%1 = %2 ;").arg(outName, i == 0 ? rhs : QStringLiteral("'b'0"));
        }
    }
    lines << QString();
    return lines.join(QLatin1Char('\n'));
}

void NewDesignDialog::updatePreview() {
    m_previewEdit->setPlainText(generatePldTemplate(spec()));
}

// ===========================================================================
// 3. NewSimDialog (.SI Scaffolder)
// ===========================================================================
QStringList NewSimDialog::expandSignalListExpr(const QString &expr) {
    QString inner = expr.trimmed();
    if (inner.startsWith(QLatin1Char('[')) && inner.endsWith(QLatin1Char(']'))) {
        inner = inner.mid(1, inner.size() - 2).trimmed();
    }

    QStringList out;
    const QStringList parts = inner.split(QLatin1Char(','), Qt::SkipEmptyParts);
    static const QRegularExpression rangeRe(
        QStringLiteral("^(!?)([A-Za-z_$][A-Za-z0-9_$]*?)(\\d+)\\s*\\.\\.\\s*(?:[A-Za-z_$][A-Za-z0-9_$]*?)?(\\d+)$"));

    for (const QString &rawPart : parts) {
        const QString tok = rawPart.trimmed();
        const QRegularExpressionMatch rm = rangeRe.match(tok);
        if (rm.hasMatch()) {
            const QString bang = rm.captured(1);
            const QString prefix = rm.captured(2);
            const int startIdx = rm.captured(3).toInt();
            const int endIdx = rm.captured(4).toInt();
            const int step = (startIdx <= endIdx) ? 1 : -1;
            for (int v = startIdx;; v += step) {
                out.append(QStringLiteral("%1%2%3").arg(bang, prefix).arg(v));
                if (v == endIdx) {
                    break;
                }
            }
        } else if (!tok.isEmpty()) {
            out.append(tok);
        }
    }
    return out;
}

PldDesignSummary NewSimDialog::parsePldForSimulation(const QString &pldText) {
    PldDesignSummary sum;
    sum.date = QDate::currentDate().toString(QStringLiteral("MM/dd/yyyy"));

    const QString clean = stripCuplComments(ErrorCatalog::normalizeConsoleText(pldText));

    // Parse ';' terminated statements (excluding SEQUENCE { ... } braces handled separately)
    auto matchHeader = [&](const QString &kw, QString &dst) {
        const QRegularExpression re(
            QStringLiteral("\\b%1\\s+([^;\\n]+)\\s*;").arg(kw),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = re.match(clean);
        if (m.hasMatch()) {
            dst = m.captured(1).trimmed();
        }
    };

    matchHeader(QStringLiteral("Name"), sum.name);
    matchHeader(QStringLiteral("Partno"), sum.partno);
    matchHeader(QStringLiteral("Date"), sum.date);
    matchHeader(QStringLiteral("Revision"), sum.revision);
    matchHeader(QStringLiteral("Designer"), sum.designer);
    matchHeader(QStringLiteral("Company"), sum.company);
    matchHeader(QStringLiteral("Assembly"), sum.assembly);
    matchHeader(QStringLiteral("Location"), sum.location);
    matchHeader(QStringLiteral("Device"), sum.device);

    // Collect FIELD declarations
    static const QRegularExpression fieldRe(
        QStringLiteral("\\bfield\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s*=\\s*(\\[[^\\]]+\\])\\s*;"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator fit = fieldRe.globalMatch(clean);
    QHash<QString, QStringList> fieldMembersByName;
    while (fit.hasNext()) {
        const QRegularExpressionMatch m = fit.next();
        PldFieldInfo fi;
        fi.name = m.captured(1).trimmed();
        fi.expr = m.captured(2).trimmed();
        for (QString mem : expandSignalListExpr(fi.expr)) {
            if (mem.startsWith(QLatin1Char('!'))) mem = mem.mid(1);
            fi.members.append(mem);
        }
        fieldMembersByName.insert(fi.name.toLower(), fi.members);
        sum.fields.append(fi);
    }

    // Identify outputs driven in equations, SEQUENCE, or TABLE
    QSet<QString> outputNamesLower;
    static const QRegularExpression lhsEqRe(
        QStringLiteral("(?:^|[;\\{\\}])\\s*(?:append\\s+)?!?(\\[[^\\]]+\\]|[A-Za-z_$][A-Za-z0-9_$]*)(?:\\.[A-Za-z]+)?\\s*="),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator eqIt = lhsEqRe.globalMatch(clean);
    while (eqIt.hasNext()) {
        const QRegularExpressionMatch m = eqIt.next();
        const QString lhs = m.captured(1).trimmed();
        for (QString s : expandSignalListExpr(lhs)) {
            if (s.startsWith(QLatin1Char('!'))) s = s.mid(1);
            if (!fieldMembersByName.contains(s.toLower())) {
                outputNamesLower.insert(s.toLower());
            } else {
                for (const QString &fm : fieldMembersByName.value(s.toLower())) {
                    outputNamesLower.insert(fm.toLower());
                }
            }
        }
    }

    static const QRegularExpression seqRe(
        QStringLiteral("\\bsequence(?:d|jk|rs|t)?\\s+(\\[[^\\]]+\\]|[A-Za-z_$][A-Za-z0-9_$]*)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator seqIt = seqRe.globalMatch(clean);
    while (seqIt.hasNext()) {
        const QString target = seqIt.next().captured(1).trimmed();
        if (fieldMembersByName.contains(target.toLower())) {
            for (const QString &fm : fieldMembersByName.value(target.toLower())) {
                outputNamesLower.insert(fm.toLower());
            }
        } else {
            for (QString s : expandSignalListExpr(target)) {
                if (s.startsWith(QLatin1Char('!'))) s = s.mid(1);
                outputNamesLower.insert(s.toLower());
            }
        }
    }

    static const QRegularExpression smOutRe(
        QStringLiteral("\\bout\\s+!?([A-Za-z_$][A-Za-z0-9_$]*)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator smOutIt = smOutRe.globalMatch(clean);
    while (smOutIt.hasNext()) {
        outputNamesLower.insert(smOutIt.next().captured(1).trimmed().toLower());
    }

    // User-defined functions: parameters assigned inside the body are outputs
    // (e.g. ADDER.PLD: function adder_slice(X, Y, Cin, Cout) { Cout = ...; }).
    // Map those parameter positions onto the actual arguments of every call site.
    static const QRegularExpression funcDefRe(
        QStringLiteral("\\bfunction\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s*\\(([^)]*)\\)\\s*\\{([^}]*)\\}"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator fdIt = funcDefRe.globalMatch(clean);
    while (fdIt.hasNext()) {
        const QRegularExpressionMatch fm = fdIt.next();
        const QString funcName = fm.captured(1).trimmed();
        const QStringList params = fm.captured(2).split(QLatin1Char(','), Qt::SkipEmptyParts);
        const QString body = fm.captured(3);

        QList<int> outParamIdx;
        for (int pi = 0; pi < params.size(); ++pi) {
            const QString pname = params.at(pi).trimmed();
            if (pname.isEmpty()) continue;
            const QRegularExpression assignRe(
                QStringLiteral("(?:^|[;{\\s])!?%1(?:\\.[A-Za-z]+)?\\s*=(?![=>])")
                    .arg(QRegularExpression::escape(pname)),
                QRegularExpression::CaseInsensitiveOption);
            if (assignRe.match(body).hasMatch()) {
                outParamIdx.append(pi);
            }
        }
        if (outParamIdx.isEmpty()) continue;

        const QRegularExpression callRe(
            QStringLiteral("\\b%1\\s*\\(([^)]*)\\)").arg(QRegularExpression::escape(funcName)),
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator cIt = callRe.globalMatch(clean);
        while (cIt.hasNext()) {
            const QRegularExpressionMatch cm = cIt.next();
            if (cm.capturedStart() >= fm.capturedStart() && cm.capturedStart() < fm.capturedEnd()) {
                continue; // the definition header itself
            }
            const QStringList callArgs = cm.captured(1).split(QLatin1Char(','));
            for (int idx : outParamIdx) {
                if (idx >= callArgs.size()) continue;
                QString arg = callArgs.at(idx).trimmed();
                if (arg.startsWith(QLatin1Char('!'))) arg = arg.mid(1).trimmed();
                static const QRegularExpression identRe(QStringLiteral("^[A-Za-z_$][A-Za-z0-9_$]*$"));
                if (!identRe.match(arg).hasMatch()) continue;
                const QString key = arg.toLower();
                if (fieldMembersByName.contains(key)) {
                    for (const QString &fmem : fieldMembersByName.value(key)) {
                        outputNamesLower.insert(fmem.toLower());
                    }
                } else {
                    outputNamesLower.insert(key);
                }
            }
        }
    }

    static const QRegularExpression tableRe(
        QStringLiteral("\\btable\\s+(\\[[^\\]]+\\]|[A-Za-z_$][A-Za-z0-9_$]*)\\s*=>\\s*(\\[[^\\]]+\\]|[A-Za-z_$][A-Za-z0-9_$]*)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator tabIt = tableRe.globalMatch(clean);
    while (tabIt.hasNext()) {
        const QString rhs = tabIt.next().captured(2).trimmed();
        if (fieldMembersByName.contains(rhs.toLower())) {
            for (const QString &fm : fieldMembersByName.value(rhs.toLower())) {
                outputNamesLower.insert(fm.toLower());
            }
        } else {
            for (QString s : expandSignalListExpr(rhs)) {
                if (s.startsWith(QLatin1Char('!'))) s = s.mid(1);
                outputNamesLower.insert(s.toLower());
            }
        }
    }

    // Parse PIN / PINNODE / NODE declarations
    static const QRegularExpression pinDeclRe(
        QStringLiteral("\\b(?:pin|pinnode|node)\\b\\s*(\\[[^\\]]+\\]|\\d+)?\\s*=\\s*(\\[[^\\]]+\\]|!?[A-Za-z_$][A-Za-z0-9_$]*)\\s*;"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator pit = pinDeclRe.globalMatch(clean);
    QSet<QString> seenLower;
    while (pit.hasNext()) {
        const QRegularExpressionMatch m = pit.next();
        const QString pinExpr = m.captured(1).trimmed();
        const QString rhsExpr = m.captured(2).trimmed();

        QStringList pinsExpanded;
        if (pinExpr.startsWith(QLatin1Char('[')) && pinExpr.endsWith(QLatin1Char(']'))) {
            const QString inner = pinExpr.mid(1, pinExpr.size() - 2);
            for (const QString &pt : inner.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString t = pt.trimmed();
                const int dots = t.indexOf(QStringLiteral(".."));
                if (dots > 0) {
                    const int a = t.left(dots).trimmed().toInt();
                    const int b = t.mid(dots + 2).trimmed().toInt();
                    const int step = (a <= b) ? 1 : -1;
                    for (int p = a;; p += step) {
                        pinsExpanded.append(QString::number(p));
                        if (p == b) break;
                    }
                } else {
                    pinsExpanded.append(t);
                }
            }
        } else if (!pinExpr.isEmpty()) {
            pinsExpanded.append(pinExpr);
        }

        const QStringList sigs = expandSignalListExpr(rhsExpr);
        for (int i = 0; i < sigs.size(); ++i) {
            QString rawSig = sigs.at(i).trimmed();
            bool activeLow = false;
            if (rawSig.startsWith(QLatin1Char('!'))) {
                activeLow = true;
                rawSig = rawSig.mid(1).trimmed();
            }
            if (rawSig.isEmpty() || seenLower.contains(rawSig.toLower())) {
                continue;
            }
            seenLower.insert(rawSig.toLower());

            PldSignalInfo si;
            si.name = rawSig;
            si.activeLow = activeLow;
            si.pinNumber = (i < pinsExpanded.size()) ? pinsExpanded.at(i) : pinExpr;
            si.isOutput = outputNamesLower.contains(rawSig.toLower());
            const QString low = rawSig.toLower();
            si.isClock = (!si.isOutput) &&
                         (low == QStringLiteral("clk") || low == QStringLiteral("clock") ||
                          (si.pinNumber == QStringLiteral("1") && low.contains(QStringLiteral("ck"))));
            si.include = true;
            sum.signalsList.append(si);
        }
    }

    return sum;
}

QString NewSimDialog::generateSiTemplate(const PldDesignSummary &summary, int vectorCount) {
    QStringList lines;
    lines << QStringLiteral("Name     %1;").arg(summary.name);
    lines << QStringLiteral("PartNo   %1;").arg(summary.partno);
    lines << QStringLiteral("Date     %1;").arg(summary.date);
    lines << QStringLiteral("Revision %1;").arg(summary.revision);
    lines << QStringLiteral("Designer %1;").arg(summary.designer);
    lines << QStringLiteral("Company  %1;").arg(summary.company);
    lines << QStringLiteral("Assembly %1;").arg(summary.assembly);
    lines << QStringLiteral("Location %1;").arg(summary.location);
    lines << QStringLiteral("Device   %1;").arg(summary.device);
    lines << QString();
    lines << QStringLiteral("/****************************************************************/");
    lines << QStringLiteral("/* Simulation Vectors generated by LinCUPL                      */");
    lines << QStringLiteral("/****************************************************************/");
    lines << QString();

    QList<PldSignalInfo> inputs;
    QList<PldSignalInfo> outputs;
    for (const PldSignalInfo &s : summary.signalsList) {
        if (!s.include) continue;
        if (s.isOutput) outputs.append(s);
        else inputs.append(s);
    }

    QStringList orderTokens;
    for (const PldSignalInfo &in : inputs) {
        orderTokens.append((in.activeLow ? QStringLiteral("!") : QString()) + in.name);
    }
    if (!inputs.isEmpty() && !outputs.isEmpty()) {
        orderTokens.append(QStringLiteral("%2"));
    }
    for (const PldSignalInfo &out : outputs) {
        orderTokens.append((out.activeLow ? QStringLiteral("!") : QString()) + out.name);
    }

    if (orderTokens.isEmpty()) {
        lines << QStringLiteral("ORDER: ;");
        lines << QString();
        lines << QStringLiteral("VECTORS:");
        return lines.join(QLatin1Char('\n'));
    }

    lines << QStringLiteral("ORDER: %1;").arg(orderTokens.join(QStringLiteral(", ")));
    lines << QString();
    lines << QStringLiteral("VECTORS:");

    const int nVec = qMax(1, vectorCount);
    // Count non-clock inputs so we can step binary stimulus patterns automatically
    int stimBits = 0;
    for (const PldSignalInfo &in : inputs) {
        if (!in.isClock) ++stimBits;
    }

    for (int v = 0; v < nVec; ++v) {
        QString row;
        int bitIdx = stimBits - 1;
        for (const PldSignalInfo &in : inputs) {
            if (in.isClock) {
                row.append(QLatin1Char('C'));
            } else {
                const int bitVal = (bitIdx >= 0 && bitIdx < 20) ? ((v >> bitIdx) & 1) : 0;
                row.append(bitVal ? QLatin1Char('1') : QLatin1Char('0'));
                --bitIdx;
            }
        }
        if (!inputs.isEmpty() && !outputs.isEmpty()) {
            row.append(QStringLiteral("  "));
        }
        for (int o = 0; o < outputs.size(); ++o) {
            // '*' lets CSIM compute actual simulated outputs without false mismatch errors
            row.append(QLatin1Char('*'));
        }
        lines << row;
    }
    lines << QString();
    return lines.join(QLatin1Char('\n'));
}

NewSimDialog::NewSimDialog(const QString &pldText,
                           const QString &defaultSiPath,
                           QWidget *parent)
    : QDialog(parent), m_parsed(parsePldForSimulation(pldText)) {
    setWindowTitle(QStringLiteral("New Simulation File (.SI) Wizard"));
    resize(840, 560);

    auto *root = new QVBoxLayout(this);

    auto *topForm = new QHBoxLayout();
    topForm->addWidget(new QLabel(QStringLiteral("Save .SI As:"), this));
    m_savePathEdit = new QLineEdit(defaultSiPath, this);
    auto *browseBtn = new QPushButton(QStringLiteral("Browse..."), this);
    connect(browseBtn, &QPushButton::clicked, this, &NewSimDialog::browseSavePath);
    topForm->addWidget(m_savePathEdit, 1);
    topForm->addWidget(browseBtn);
    topForm->addSpacing(12);
    topForm->addWidget(new QLabel(QStringLiteral("Initial Vectors:"), this));
    m_vecCountSpin = new QSpinBox(this);
    m_vecCountSpin->setRange(1, 256);
    m_vecCountSpin->setValue(8);
    topForm->addWidget(m_vecCountSpin);
    root->addLayout(topForm);

    auto *splitter = new QSplitter(Qt::Horizontal, this);

    m_sigTable = new QTableWidget(m_parsed.signalsList.size(), 5, splitter);
    m_sigTable->setHorizontalHeaderLabels({
        QStringLiteral("Include"),
        QStringLiteral("Signal"),
        QStringLiteral("Pin"),
        QStringLiteral("Active Low (!)"),
        QStringLiteral("Role")
    });
    m_sigTable->horizontalHeader()->setStretchLastSection(true);

    for (int r = 0; r < m_parsed.signalsList.size(); ++r) {
        const PldSignalInfo &s = m_parsed.signalsList.at(r);

        auto *incItem = new QTableWidgetItem();
        incItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        incItem->setCheckState(s.include ? Qt::Checked : Qt::Unchecked);
        m_sigTable->setItem(r, 0, incItem);

        m_sigTable->setItem(r, 1, new QTableWidgetItem(s.name));
        auto *pinItem = new QTableWidgetItem(s.pinNumber);
        pinItem->setFlags(pinItem->flags() & ~Qt::ItemIsEditable);
        m_sigTable->setItem(r, 2, pinItem);

        auto *lowItem = new QTableWidgetItem();
        lowItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        lowItem->setCheckState(s.activeLow ? Qt::Checked : Qt::Unchecked);
        m_sigTable->setItem(r, 3, lowItem);

        auto *roleCombo = new QComboBox(m_sigTable);
        roleCombo->addItems({
            QStringLiteral("Input (0/1)"),
            QStringLiteral("Clock Input (C)"),
            QStringLiteral("Output (* / L / H)")
        });
        if (s.isOutput) roleCombo->setCurrentIndex(2);
        else if (s.isClock) roleCombo->setCurrentIndex(1);
        else roleCombo->setCurrentIndex(0);
        connect(roleCombo, &QComboBox::currentIndexChanged, this, [this](int) { rebuildPreview(); });
        m_sigTable->setCellWidget(r, 4, roleCombo);
    }

    m_previewEdit = new QPlainTextEdit(splitter);
    m_previewEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    new CuplHighlighter(m_previewEdit->document());

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 3);
    root->addWidget(splitter, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_vecCountSpin, &QSpinBox::valueChanged, this, [this](int) { rebuildPreview(); });
    connect(m_sigTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *) { rebuildPreview(); });

    rebuildPreview();
}

PldDesignSummary NewSimDialog::currentSummaryFromUi() const {
    PldDesignSummary s = m_parsed;
    s.signalsList.clear();
    for (int r = 0; r < m_sigTable->rowCount(); ++r) {
        PldSignalInfo info;
        info.include = m_sigTable->item(r, 0) && m_sigTable->item(r, 0)->checkState() == Qt::Checked;
        info.name = m_sigTable->item(r, 1) ? m_sigTable->item(r, 1)->text().trimmed() : QString();
        info.pinNumber = m_sigTable->item(r, 2) ? m_sigTable->item(r, 2)->text() : QString();
        info.activeLow = m_sigTable->item(r, 3) && m_sigTable->item(r, 3)->checkState() == Qt::Checked;
        auto *combo = qobject_cast<QComboBox *>(m_sigTable->cellWidget(r, 4));
        const int role = combo ? combo->currentIndex() : 0;
        info.isClock = (role == 1);
        info.isOutput = (role == 2);
        s.signalsList.append(info);
    }
    return s;
}

void NewSimDialog::browseSavePath() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save Simulation (.SI) File"), m_savePathEdit->text(),
        QStringLiteral("CUPL Simulation Files (*.si *.SI);;All Files (*)"));
    if (!path.isEmpty()) {
        m_savePathEdit->setText(path);
    }
}

void NewSimDialog::rebuildPreview() {
    m_previewEdit->setPlainText(generateSiTemplate(currentSummaryFromUi(), m_vecCountSpin->value()));
}

QString NewSimDialog::siFilePath() const {
    return m_savePathEdit->text().trimmed();
}

QString NewSimDialog::generatedSiText() const {
    return m_previewEdit->toPlainText();
}

// ===========================================================================
// 4. ErrorViewerDialog (CuplErr.txt Catalog)
// ===========================================================================
ErrorViewerDialog::ErrorViewerDialog(const ErrorCatalog *catalog,
                                     const QString &initialCode,
                                     QWidget *parent)
    : QDialog(parent), m_catalog(catalog) {
    setWindowTitle(QStringLiteral("CUPL && CSIM Error Message Reference (CuplErr.txt)"));
    resize(840, 540);

    auto *root = new QVBoxLayout(this);

    auto *filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(QStringLiteral("Module:"), this));
    m_moduleCombo = new QComboBox(this);
    if (m_catalog) {
        m_moduleCombo->addItems(m_catalog->modules());
    }
    filterRow->addWidget(m_moduleCombo);

    filterRow->addWidget(new QLabel(QStringLiteral("Severity:"), this));
    m_severityCombo = new QComboBox(this);
    m_severityCombo->addItems({
        QStringLiteral("All Severities"),
        QStringLiteral("Fatal"),
        QStringLiteral("Error"),
        QStringLiteral("Warning")
    });
    filterRow->addWidget(m_severityCombo);

    filterRow->addWidget(new QLabel(QStringLiteral("Search:"), this));
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setPlaceholderText(QStringLiteral("Search error code (e.g. 0007ca, 0019sa) or keyword..."));
    filterRow->addWidget(m_searchEdit, 1);
    root->addLayout(filterRow);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_tree = new QTreeWidget(splitter);
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setHeaderLabels({
        QStringLiteral("Code"),
        QStringLiteral("Severity"),
        QStringLiteral("Module"),
        QStringLiteral("Summary")
    });
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    m_detailBrowser = new QTextBrowser(splitter);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    auto *closeBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(closeBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(closeBox);

    connect(m_moduleCombo, &QComboBox::currentIndexChanged, this, [this](int) { refreshList(); });
    connect(m_severityCombo, &QComboBox::currentIndexChanged, this, [this](int) { refreshList(); });
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &) { refreshList(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &ErrorViewerDialog::onSelectionChanged);

    refreshList();
    if (!initialCode.isEmpty()) {
        selectErrorCode(initialCode);
    }
}

void ErrorViewerDialog::refreshList() {
    m_tree->clear();
    if (!m_catalog) {
        return;
    }
    const QList<CuplErrorEntry> list = m_catalog->search(
        m_moduleCombo->currentText(),
        m_severityCombo->currentText(),
        m_searchEdit->text());

    for (const CuplErrorEntry &e : list) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QStringLiteral("[%1]").arg(e.code));
        item->setText(1, e.severity);
        item->setText(2, e.moduleName);
        item->setText(3, e.summary);
        item->setData(0, Qt::UserRole, e.code);
    }
    if (m_tree->topLevelItemCount() > 0) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    } else {
        m_detailBrowser->clear();
    }
}

void ErrorViewerDialog::selectErrorCode(const QString &code) {
    if (!m_catalog) {
        return;
    }
    const CuplErrorEntry target = m_catalog->lookup(code);
    const QString targetKey = target.isValid() ? target.code : code.trimmed().toLower();
    m_moduleCombo->setCurrentIndex(0);
    m_severityCombo->setCurrentIndex(0);
    m_searchEdit->clear();
    refreshList();

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_tree->topLevelItem(i);
        if (it->data(0, Qt::UserRole).toString() == targetKey) {
            m_tree->setCurrentItem(it);
            m_tree->scrollToItem(it);
            break;
        }
    }
}

void ErrorViewerDialog::onSelectionChanged() {
    const QList<QTreeWidgetItem *> sel = m_tree->selectedItems();
    if (sel.isEmpty() || !m_catalog) {
        return;
    }
    const CuplErrorEntry e = m_catalog->lookup(sel.first()->data(0, Qt::UserRole).toString());
    if (!e.isValid()) {
        return;
    }
    m_detailBrowser->setHtml(QStringLiteral(
        "<h3 style='margin-bottom:4px;'>[%1] %2</h3>"
        "<p><b>Severity:</b> %3 &nbsp;&nbsp;|&nbsp;&nbsp; <b>Module:</b> %4</p>"
        "<hr/><p style='white-space:pre-wrap;'>%5</p>")
        .arg(e.code.toHtmlEscaped(),
             e.summary.toHtmlEscaped(),
             e.severity.toHtmlEscaped(),
             e.moduleName.toHtmlEscaped(),
             e.explanation.toHtmlEscaped()));
}

// ===========================================================================
// 5. BackpinDialog
// ===========================================================================
QStringList BackpinRequest::buildArgs() const {
    QStringList args;
    if (!customBackupFile.isEmpty()) {
        args << QStringLiteral("-b") << customBackupFile;
    }
    if (!logFile.isEmpty()) {
        args << QStringLiteral("-l") << logFile;
    }
    if (floatAllPins) {
        args << QStringLiteral("-f");
    } else if (pinNumbersOnly) {
        args << QStringLiteral("-p");
    }
    if (!libraryPath.isEmpty()) {
        args << QStringLiteral("-u") << libraryPath;
    }
    args << deviceMnemonic.trimmed().toLower();
    args << sourcePldPath;
    return args;
}

BackpinDialog::BackpinDialog(DeviceLibrary *library,
                             const QString &pldPath,
                             const QString &deviceMnemonic,
                             QWidget *parent)
    : QDialog(parent), m_library(library) {
    setWindowTitle(QStringLiteral("Backpin - Pin Placement Back-Annotation"));
    resize(620, 380);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    auto *pldRow = new QHBoxLayout();
    m_pldEdit = new QLineEdit(pldPath, this);
    auto *pldBtn = new QPushButton(QStringLiteral("Browse..."), this);
    connect(pldBtn, &QPushButton::clicked, this, &BackpinDialog::browsePld);
    pldRow->addWidget(m_pldEdit, 1);
    pldRow->addWidget(pldBtn);
    form->addRow(QStringLiteral("Source .PLD File:"), pldRow);

    auto *devRow = new QHBoxLayout();
    m_devEdit = new QLineEdit(deviceMnemonic.isEmpty() ? QStringLiteral("g16v8a") : deviceMnemonic, this);
    auto *devBtn = new QPushButton(QStringLiteral("Devices..."), this);
    connect(devBtn, &QPushButton::clicked, this, &BackpinDialog::browseDevice);
    devRow->addWidget(m_devEdit, 1);
    devRow->addWidget(devBtn);
    form->addRow(QStringLiteral("Target Device:"), devRow);

    auto *libRow = new QHBoxLayout();
    m_libEdit = new QLineEdit(library ? library->libraryPath() : ToolPaths::activeDeviceLibrary(), this);
    auto *libBtn = new QPushButton(QStringLiteral("Browse..."), this);
    connect(libBtn, &QPushButton::clicked, this, &BackpinDialog::browseLib);
    libRow->addWidget(m_libEdit, 1);
    libRow->addWidget(libBtn);
    form->addRow(QStringLiteral("Device Library (.dl):"), libRow);

    root->addLayout(form);

    auto *modeGroup = new QGroupBox(QStringLiteral("Backpin Operation Mode"), this);
    auto *modeVBox = new QVBoxLayout(modeGroup);
    m_rbAnnotate = new QRadioButton(
        QStringLiteral("Back-annotate pins from fitter placement (.plc / .ppn) into .PLD"), modeGroup);
    m_rbAnnotate->setChecked(true);
    m_chkPinOnly = new QCheckBox(QStringLiteral("-p : Place pin numbers only (do not update pinnodes)"), modeGroup);
    m_rbFloat = new QRadioButton(
        QStringLiteral("-f : Float (clear) all assigned pin numbers in .PLD (creates .bak backup)"), modeGroup);
    modeVBox->addWidget(m_rbAnnotate);
    modeVBox->addWidget(m_chkPinOnly);
    modeVBox->addWidget(m_rbFloat);
    root->addWidget(modeGroup);

    auto *optForm = new QFormLayout();
    m_bakEdit = new QLineEdit(this);
    m_bakEdit->setPlaceholderText(QStringLiteral("(Default: <design>.bak)"));
    m_logEdit = new QLineEdit(this);
    m_logEdit->setPlaceholderText(QStringLiteral("(Default: stdout to LinCUPL Build Log)"));
    optForm->addRow(QStringLiteral("Backup Filename (-b):"), m_bakEdit);
    optForm->addRow(QStringLiteral("Log Filename (-l):"), m_logEdit);
    root->addLayout(optForm);

    auto *prevRow = new QHBoxLayout();
    prevRow->addWidget(new QLabel(QStringLiteral("Command:"), this));
    m_previewEdit = new QLineEdit(this);
    m_previewEdit->setReadOnly(true);
    prevRow->addWidget(m_previewEdit, 1);
    root->addLayout(prevRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Run Backpin"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_pldEdit->text().trimmed().isEmpty() || m_devEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Missing Input"),
                                 QStringLiteral("Please specify both the target device and source .PLD file."));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    for (QLineEdit *le : {m_pldEdit, m_devEdit, m_libEdit, m_bakEdit, m_logEdit}) {
        connect(le, &QLineEdit::textChanged, this, &BackpinDialog::updatePreview);
    }
    connect(m_rbAnnotate, &QRadioButton::toggled, this, [this](bool on) {
        m_chkPinOnly->setEnabled(on);
        updatePreview();
    });
    connect(m_chkPinOnly, &QCheckBox::toggled, this, &BackpinDialog::updatePreview);

    updatePreview();
}

void BackpinDialog::browsePld() {
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select Source .PLD File"), m_pldEdit->text(),
        QStringLiteral("CUPL Source Files (*.pld *.PLD);;All Files (*)"));
    if (!f.isEmpty()) m_pldEdit->setText(f);
}

void BackpinDialog::browseDevice() {
    DeviceBrowserDialog dlg(m_library, m_devEdit->text(), false, this);
    if (dlg.exec() == QDialog::Accepted && !dlg.selectedMnemonic().isEmpty()) {
        m_devEdit->setText(dlg.selectedMnemonic());
    }
}

void BackpinDialog::browseLib() {
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select Device Library (.dl)"), m_libEdit->text(),
        QStringLiteral("CUPL Libraries (*.dl *.DL);;All Files (*)"));
    if (!f.isEmpty()) m_libEdit->setText(f);
}

BackpinRequest BackpinDialog::request() const {
    BackpinRequest r;
    r.sourcePldPath = m_pldEdit->text().trimmed();
    r.deviceMnemonic = m_devEdit->text().trimmed();
    r.libraryPath = m_libEdit->text().trimmed();
    r.floatAllPins = m_rbFloat->isChecked();
    r.pinNumbersOnly = m_rbAnnotate->isChecked() && m_chkPinOnly->isChecked();
    r.customBackupFile = m_bakEdit->text().trimmed();
    r.logFile = m_logEdit->text().trimmed();
    return r;
}

void BackpinDialog::updatePreview() {
    m_previewEdit->setText(QStringLiteral("backpin ") + request().buildArgs().join(QLatin1Char(' ')));
}

// ===========================================================================
// 6. FindReplaceDialog
// ===========================================================================
FindReplaceDialog::FindReplaceDialog(std::function<CodeEditor *()> editorProvider,
                                     QWidget *parent)
    : QDialog(parent), m_editorProvider(std::move(editorProvider)) {
    setWindowTitle(QStringLiteral("Find && Replace"));
    resize(440, 200);

    auto *grid = new QGridLayout(this);
    m_findEdit = new QLineEdit(this);
    m_replaceEdit = new QLineEdit(this);
    m_chkCase = new QCheckBox(QStringLiteral("Match case"), this);
    m_chkWhole = new QCheckBox(QStringLiteral("Whole words"), this);
    m_statusLabel = new QLabel(this);

    auto *btnNext = new QPushButton(QStringLiteral("Find Next"), this);
    auto *btnPrev = new QPushButton(QStringLiteral("Find Previous"), this);
    auto *btnRep = new QPushButton(QStringLiteral("Replace"), this);
    auto *btnRepAll = new QPushButton(QStringLiteral("Replace All"), this);
    auto *btnClose = new QPushButton(QStringLiteral("Close"), this);

    btnNext->setDefault(true);

    grid->addWidget(new QLabel(QStringLiteral("Find what:"), this), 0, 0);
    grid->addWidget(m_findEdit, 0, 1);
    grid->addWidget(btnNext, 0, 2);

    grid->addWidget(new QLabel(QStringLiteral("Replace with:"), this), 1, 0);
    grid->addWidget(m_replaceEdit, 1, 1);
    grid->addWidget(btnPrev, 1, 2);

    auto *chkRow = new QHBoxLayout();
    chkRow->addWidget(m_chkCase);
    chkRow->addWidget(m_chkWhole);
    chkRow->addStretch();
    grid->addLayout(chkRow, 2, 0, 1, 2);
    grid->addWidget(btnRep, 2, 2);

    grid->addWidget(m_statusLabel, 3, 0, 1, 2);
    grid->addWidget(btnRepAll, 3, 2);
    grid->addWidget(btnClose, 4, 2);

    connect(btnNext, &QPushButton::clicked, this, &FindReplaceDialog::onFindNext);
    connect(btnPrev, &QPushButton::clicked, this, &FindReplaceDialog::onFindPrev);
    connect(btnRep, &QPushButton::clicked, this, &FindReplaceDialog::onReplaceOne);
    connect(btnRepAll, &QPushButton::clicked, this, &FindReplaceDialog::onReplaceAll);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::close);
}

void FindReplaceDialog::focusFind(const QString &seedText, bool showReplace) {
    if (!seedText.isEmpty()) {
        m_findEdit->setText(seedText);
    }
    show();
    raise();
    activateWindow();
    if (showReplace && !m_findEdit->text().isEmpty()) {
        m_replaceEdit->setFocus();
        m_replaceEdit->selectAll();
    } else {
        m_findEdit->setFocus();
        m_findEdit->selectAll();
    }
}

void FindReplaceDialog::onFindNext() {
    CodeEditor *ed = m_editorProvider ? m_editorProvider() : nullptr;
    if (!ed) return;
    const bool ok = ed->findNextText(m_findEdit->text(), m_chkCase->isChecked(), m_chkWhole->isChecked(), false);
    m_statusLabel->setText(ok ? QString() : QStringLiteral("No match found."));
}

void FindReplaceDialog::onFindPrev() {
    CodeEditor *ed = m_editorProvider ? m_editorProvider() : nullptr;
    if (!ed) return;
    const bool ok = ed->findNextText(m_findEdit->text(), m_chkCase->isChecked(), m_chkWhole->isChecked(), true);
    m_statusLabel->setText(ok ? QString() : QStringLiteral("No match found."));
}

void FindReplaceDialog::onReplaceOne() {
    CodeEditor *ed = m_editorProvider ? m_editorProvider() : nullptr;
    if (!ed || ed->isReadOnly()) {
        m_statusLabel->setText(QStringLiteral("Current tab is read-only."));
        return;
    }
    const QString needle = m_findEdit->text();
    if (needle.isEmpty()) return;

    QTextCursor cur = ed->textCursor();
    if (cur.hasSelection()) {
        const Qt::CaseSensitivity cs = m_chkCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
        if (cur.selectedText().compare(needle, cs) == 0) {
            cur.insertText(m_replaceEdit->text());
        }
    }
    onFindNext();
}

void FindReplaceDialog::onReplaceAll() {
    CodeEditor *ed = m_editorProvider ? m_editorProvider() : nullptr;
    if (!ed || ed->isReadOnly()) {
        m_statusLabel->setText(QStringLiteral("Current tab is read-only."));
        return;
    }
    const int n = ed->replaceAllText(m_findEdit->text(), m_replaceEdit->text(),
                                     m_chkCase->isChecked(), m_chkWhole->isChecked());
    m_statusLabel->setText(QStringLiteral("Replaced %1 occurrence(s).").arg(n));
}

// ===========================================================================
// 7. TableEditorDialog (Binary Table Editor)
// ===========================================================================
namespace {

constexpr int kMaxTableInputBits = 8; // 256 rows

QString toBinary(int value, int width) {
    return QStringLiteral("%1").arg(value, width, 2, QLatin1Char('0'));
}

QString normalizeBits(const QString &raw, int width) {
    QString bits;
    for (const QChar c : raw.trimmed().toUpper()) {
        if (c == QLatin1Char('0') || c == QLatin1Char('1') || c == QLatin1Char('X')) {
            bits.append(c);
        }
    }
    if (bits.size() > width) {
        bits = bits.right(width);
    }
    return bits.rightJustified(width, QLatin1Char('0'));
}

} // namespace

QString TableEditorDialog::generateTable(const QStringList &inputs,
                                         const QStringList &outputs,
                                         const QStringList &outputValues) {
    if (inputs.isEmpty() || outputs.isEmpty()) {
        return QString();
    }
    const int nIn = qMin<int>(inputs.size(), kMaxTableInputBits);
    const int nOut = outputs.size();
    const int rows = 1 << nIn;

    QStringList lines;
    lines << QStringLiteral("TABLE [%1] => [%2] {")
                 .arg(inputs.mid(0, nIn).join(QStringLiteral(", ")), outputs.join(QStringLiteral(", ")));
    for (int r = 0; r < rows; ++r) {
        const QString outBits = normalizeBits(r < outputValues.size() ? outputValues.at(r) : QString(), nOut);
        lines << QStringLiteral("    'b'%1 => 'b'%2;").arg(toBinary(r, nIn), outBits);
    }
    lines << QStringLiteral("}");
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

TableEditorDialog::TableEditorDialog(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Binary Table Editor"));
    resize(720, 520);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    m_inEdit = new QLineEdit(QStringLiteral("a, b"), this);
    m_inEdit->setPlaceholderText(QStringLiteral("Input signals, e.g. A3..0  or  sel, a, b"));
    m_outEdit = new QLineEdit(QStringLiteral("y"), this);
    m_outEdit->setPlaceholderText(QStringLiteral("Output signals, e.g. Y1..0"));
    form->addRow(QStringLiteral("Input variables:"), m_inEdit);
    form->addRow(QStringLiteral("Output variables:"), m_outEdit);
    root->addLayout(form);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    m_table = new QTableWidget(0, 2, splitter);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Inputs (binary)"), QStringLiteral("Outputs (0/1/X)")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_preview = new QPlainTextEdit(splitter);
    m_preview->setReadOnly(true);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    new CuplHighlighter(m_preview->document());
    root->addWidget(splitter, 1);

    m_status = new QLabel(this);
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Insert Table"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_inEdit, &QLineEdit::editingFinished, this, &TableEditorDialog::rebuildRows);
    connect(m_outEdit, &QLineEdit::editingFinished, this, &TableEditorDialog::rebuildRows);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *) { updatePreview(); });

    rebuildRows();
}

QStringList TableEditorDialog::inputSignals() const {
    return NewSimDialog::expandSignalListExpr(m_inEdit->text());
}

QStringList TableEditorDialog::outputSignals() const {
    return NewSimDialog::expandSignalListExpr(m_outEdit->text());
}

void TableEditorDialog::setSignals(const QString &inputsExpr, const QString &outputsExpr) {
    m_inEdit->setText(inputsExpr);
    m_outEdit->setText(outputsExpr);
    rebuildRows();
}

void TableEditorDialog::setOutputValue(int row, const QString &bits) {
    if (row >= 0 && row < m_table->rowCount() && m_table->item(row, 1)) {
        m_table->item(row, 1)->setText(bits);
    }
}

void TableEditorDialog::rebuildRows() {
    const QStringList ins = inputSignals();
    const QStringList outs = outputSignals();
    const int nIn = qMin<int>(ins.size(), kMaxTableInputBits);
    const int nOut = qMax<int>(1, outs.size());

    // Preserve previously entered output values where the row still exists
    QStringList previous;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        previous << (m_table->item(r, 1) ? m_table->item(r, 1)->text() : QString());
    }

    const int rows = ins.isEmpty() ? 0 : (1 << nIn);
    m_table->blockSignals(true);
    m_table->setRowCount(rows);
    for (int r = 0; r < rows; ++r) {
        auto *inItem = new QTableWidgetItem(toBinary(r, nIn));
        inItem->setFlags(inItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(r, 0, inItem);
        const QString val = r < previous.size() ? previous.at(r) : QString();
        m_table->setItem(r, 1, new QTableWidgetItem(normalizeBits(val, nOut)));
    }
    m_table->blockSignals(false);

    if (ins.size() > kMaxTableInputBits) {
        m_status->setText(QStringLiteral("Only the first %1 input variables are tabulated (256 rows).")
                              .arg(kMaxTableInputBits));
    } else {
        m_status->setText(QStringLiteral("%1 input(s), %2 output(s), %3 row(s).")
                              .arg(ins.size()).arg(outs.size()).arg(rows));
    }
    updatePreview();
}

QString TableEditorDialog::generatedTable() const {
    QStringList values;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        values << (m_table->item(r, 1) ? m_table->item(r, 1)->text() : QString());
    }
    return generateTable(inputSignals(), outputSignals(), values);
}

void TableEditorDialog::updatePreview() {
    m_preview->setPlainText(generatedTable());
}

// ===========================================================================
// 8. MacroDialog (Define / Create / Reference CUPL macros)
// ===========================================================================
QList<CuplMacroDef> MacroDialog::scanMacros(const QString &text, const QString &sourceFile) {
    QList<CuplMacroDef> out;
    static const QRegularExpression defRe(
        QStringLiteral("^\\s*\\$macro\\s+([A-Za-z_$][A-Za-z0-9_$]*)([^\\n]*)$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = defRe.globalMatch(stripCuplComments(ErrorCatalog::normalizeConsoleText(text)));
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        CuplMacroDef def;
        def.name = m.captured(1);
        QString paramText = m.captured(2);
        paramText.replace(QLatin1Char(','), QLatin1Char(' '))
            .replace(QLatin1Char('('), QLatin1Char(' '))
            .replace(QLatin1Char(')'), QLatin1Char(' '))
            .replace(QLatin1Char(';'), QLatin1Char(' '));
        def.params = paramText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        def.sourceFile = sourceFile;
        out.append(def);
    }
    return out;
}

QList<CuplMacroDef> MacroDialog::discoverMacros(const QString &currentText, const QString &baseDir) {
    QList<CuplMacroDef> all = scanMacros(currentText);
    QSet<QString> seenNames;
    for (const CuplMacroDef &d : all) seenNames.insert(d.name.toLower());

    auto addFile = [&](const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return;
        for (const CuplMacroDef &d : scanMacros(QString::fromLatin1(f.readAll()), path)) {
            if (!seenNames.contains(d.name.toLower())) {
                seenNames.insert(d.name.toLower());
                all.append(d);
            }
        }
    };

    // Files referenced via $include, resolved case-insensitively next to the design
    static const QRegularExpression incRe(
        QStringLiteral("^\\s*\\$include\\s+\"?([^\"\\s;]+)\"?"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = incRe.globalMatch(currentText);
    const QDir dir(baseDir.isEmpty() ? QDir::currentPath() : baseDir);
    while (it.hasNext()) {
        const QString inc = it.next().captured(1);
        QFileInfo direct(dir.filePath(inc));
        if (direct.exists()) {
            addFile(direct.absoluteFilePath());
            continue;
        }
        for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
            if (fi.fileName().compare(QFileInfo(inc).fileName(), Qt::CaseInsensitive) == 0) {
                addFile(fi.absoluteFilePath());
                break;
            }
        }
    }

    // Shipped macro libraries (Macro/*.MAC)
    const QString macroDir = ToolPaths::findMacroDir();
    if (!macroDir.isEmpty()) {
        for (const QFileInfo &fi : QDir(macroDir).entryInfoList(QDir::Files, QDir::Name)) {
            const QString ext = fi.suffix().toLower();
            if (ext == QStringLiteral("mac") || ext == QStringLiteral("m")) {
                addFile(fi.absoluteFilePath());
            }
        }
    }
    return all;
}

QString MacroDialog::formatDefinition(const QString &name, const QStringList &params, const QString &body) {
    QString b = body;
    while (b.endsWith(QLatin1Char('\n'))) b.chop(1);
    return QStringLiteral("$MACRO %1 %2\n%3\n$MEND\n").arg(name, params.join(QLatin1Char(' ')), b);
}

QString MacroDialog::formatReference(const CuplMacroDef &def, const QStringList &args) {
    QStringList filled;
    for (int i = 0; i < def.params.size(); ++i) {
        const QString a = i < args.size() ? args.at(i).trimmed() : QString();
        filled << (a.isEmpty() ? QStringLiteral("NC") : a);
    }
    return QStringLiteral("%1( %2 );\n").arg(def.name, filled.join(QStringLiteral(", ")));
}

MacroDialog::MacroDialog(Mode mode, const QString &currentText, const QString &baseDir, QWidget *parent)
    : QDialog(parent), m_mode(mode), m_currentText(currentText) {
    auto *root = new QVBoxLayout(this);
    resize(700, 520);

    if (mode == Mode::Reference) {
        setWindowTitle(QStringLiteral("Reference CUPL Macro"));
        m_macros = discoverMacros(currentText, baseDir);

        auto *row = new QHBoxLayout();
        row->addWidget(new QLabel(QStringLiteral("Macro:"), this));
        m_macroCombo = new QComboBox(this);
        for (const CuplMacroDef &d : m_macros) {
            m_macroCombo->addItem(QStringLiteral("%1  (%2 params)  %3")
                                      .arg(d.name)
                                      .arg(d.params.size())
                                      .arg(d.sourceFile.isEmpty() ? QStringLiteral("[this file]")
                                                                  : QFileInfo(d.sourceFile).fileName()));
        }
        row->addWidget(m_macroCombo, 1);
        root->addLayout(row);

        m_argTable = new QTableWidget(0, 2, this);
        m_argTable->setHorizontalHeaderLabels({QStringLiteral("Parameter"), QStringLiteral("Argument")});
        m_argTable->horizontalHeader()->setStretchLastSection(true);
        root->addWidget(m_argTable, 1);

        m_chkInclude = new QCheckBox(QStringLiteral("Also insert $include for macros defined in an external library"), this);
        m_chkInclude->setChecked(true);
        root->addWidget(m_chkInclude);

        connect(m_macroCombo, &QComboBox::currentIndexChanged, this, &MacroDialog::onMacroChanged);
        connect(m_argTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *) { updatePreview(); });
        connect(m_chkInclude, &QCheckBox::toggled, this, &MacroDialog::updatePreview);
    } else {
        setWindowTitle(mode == Mode::Define ? QStringLiteral("Define CUPL Macro")
                                            : QStringLiteral("Create CUPL Macro Library Entry"));
        auto *form = new QFormLayout();
        m_nameEdit = new QLineEdit(QStringLiteral("MyMacro"), this);
        m_paramsEdit = new QLineEdit(QStringLiteral("A B Y"), this);
        m_paramsEdit->setPlaceholderText(QStringLiteral("Space-separated formal parameters"));
        form->addRow(QStringLiteral("Macro name:"), m_nameEdit);
        form->addRow(QStringLiteral("Parameters:"), m_paramsEdit);
        if (mode == Mode::Create) {
            auto *libRow = new QHBoxLayout();
            m_libEdit = new QLineEdit(QDir(baseDir.isEmpty() ? QDir::currentPath() : baseDir)
                                          .filePath(QStringLiteral("macros.m")), this);
            auto *btn = new QPushButton(QStringLiteral("Browse..."), this);
            connect(btn, &QPushButton::clicked, this, &MacroDialog::browseLibrary);
            libRow->addWidget(m_libEdit, 1);
            libRow->addWidget(btn);
            form->addRow(QStringLiteral("Library file (.m):"), libRow);
        }
        root->addLayout(form);
        root->addWidget(new QLabel(QStringLiteral("Macro body (CUPL equations using the parameters):"), this));
        m_bodyEdit = new QPlainTextEdit(QStringLiteral("Y = A & B;"), this);
        m_bodyEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        new CuplHighlighter(m_bodyEdit->document());
        root->addWidget(m_bodyEdit, 1);

        connect(m_nameEdit, &QLineEdit::textChanged, this, &MacroDialog::updatePreview);
        connect(m_paramsEdit, &QLineEdit::textChanged, this, &MacroDialog::updatePreview);
        connect(m_bodyEdit, &QPlainTextEdit::textChanged, this, &MacroDialog::updatePreview);
        if (m_libEdit) connect(m_libEdit, &QLineEdit::textChanged, this, &MacroDialog::updatePreview);
    }

    root->addWidget(new QLabel(QStringLiteral("Text inserted into the editor:"), this));
    m_preview = new QPlainTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setMaximumHeight(140);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    new CuplHighlighter(m_preview->document());
    root->addWidget(m_preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_mode == Mode::Reference && m_macros.isEmpty()) {
            QMessageBox::information(this, windowTitle(), QStringLiteral("No $MACRO definitions were found."));
            return;
        }
        if (m_mode != Mode::Reference && m_nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("Please enter a macro name."));
            return;
        }
        if (m_mode == Mode::Create) {
            const QString lib = m_libEdit->text().trimmed();
            QFile f(lib);
            if (lib.isEmpty() || !f.open(QIODevice::Append | QIODevice::Text)) {
                QMessageBox::warning(this, windowTitle(), QStringLiteral("Cannot write macro library:\n%1").arg(lib));
                return;
            }
            f.write("\n");
            f.write(formatDefinition(m_nameEdit->text().trimmed(),
                                     m_paramsEdit->text().split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts),
                                     m_bodyEdit->toPlainText()).toLatin1());
            m_writtenLibrary = QFileInfo(lib).absoluteFilePath();
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    if (m_mode == Mode::Reference && !m_macros.isEmpty()) {
        onMacroChanged(0);
    }
    updatePreview();
}

void MacroDialog::browseLibrary() {
    const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Macro Library File"), m_libEdit->text(),
                                                   QStringLiteral("CUPL Macro Files (*.m *.M *.mac *.MAC);;All Files (*)"));
    if (!f.isEmpty()) m_libEdit->setText(f);
}

void MacroDialog::onMacroChanged(int index) {
    if (!m_argTable || index < 0 || index >= m_macros.size()) return;
    const CuplMacroDef &d = m_macros.at(index);
    m_argTable->blockSignals(true);
    m_argTable->setRowCount(d.params.size());
    for (int i = 0; i < d.params.size(); ++i) {
        auto *p = new QTableWidgetItem(d.params.at(i));
        p->setFlags(p->flags() & ~Qt::ItemIsEditable);
        m_argTable->setItem(i, 0, p);
        m_argTable->setItem(i, 1, new QTableWidgetItem(QStringLiteral("NC")));
    }
    m_argTable->blockSignals(false);
    updatePreview();
}

QString MacroDialog::generatedText() const {
    if (m_mode == Mode::Define) {
        return formatDefinition(m_nameEdit->text().trimmed(),
                                m_paramsEdit->text().split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts),
                                m_bodyEdit->toPlainText());
    }
    if (m_mode == Mode::Create) {
        const QString lib = m_libEdit ? QFileInfo(m_libEdit->text().trimmed()).fileName() : QString();
        return lib.isEmpty() ? QString() : QStringLiteral("$include %1\n").arg(lib);
    }

    const int idx = m_macroCombo ? m_macroCombo->currentIndex() : -1;
    if (idx < 0 || idx >= m_macros.size()) return QString();
    const CuplMacroDef &d = m_macros.at(idx);
    QStringList args;
    for (int i = 0; i < m_argTable->rowCount(); ++i) {
        args << (m_argTable->item(i, 1) ? m_argTable->item(i, 1)->text() : QString());
    }
    QString text;
    if (!d.sourceFile.isEmpty() && m_chkInclude && m_chkInclude->isChecked()) {
        const QString fname = QFileInfo(d.sourceFile).fileName();
        static const QRegularExpression anyInc(QStringLiteral("\\$include\\s+\"?([^\"\\s;]+)"),
                                               QRegularExpression::CaseInsensitiveOption);
        bool already = false;
        QRegularExpressionMatchIterator it = anyInc.globalMatch(m_currentText);
        while (it.hasNext()) {
            if (QFileInfo(it.next().captured(1)).fileName().compare(fname, Qt::CaseInsensitive) == 0) {
                already = true;
                break;
            }
        }
        if (!already) {
            text += QStringLiteral("$include %1\n").arg(d.sourceFile);
        }
    }
    text += formatReference(d, args);
    return text;
}

void MacroDialog::updatePreview() {
    if (m_preview) m_preview->setPlainText(generatedText());
}
