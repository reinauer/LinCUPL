#include "SelfTest.h"
#include "MainWindow.h"
#include "SimDocument.h"
#include "WaveformView.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cstdio>

namespace {

struct TestStats {
    int passed = 0;
    int failed = 0;

    void check(bool cond, const QString &desc, const QString &detail = QString()) {
        if (cond) {
            ++passed;
            std::fprintf(stdout, "  [PASS] %s\n", desc.toUtf8().constData());
        } else {
            ++failed;
            if (detail.isEmpty()) {
                std::fprintf(stderr, "  [FAIL] %s\n", desc.toUtf8().constData());
            } else {
                std::fprintf(stderr, "  [FAIL] %s -- %s\n",
                             desc.toUtf8().constData(),
                             detail.toUtf8().constData());
            }
        }
    }
};

QString locateAtmelExamplesDir() {
    const QString envShare = qEnvironmentVariable("LINCUPL_SHARE");
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    if (!envShare.isEmpty()) {
        candidates.append(QDir(envShare).filePath(QStringLiteral("Examples/Atmel")));
    }
    candidates.append({
        QDir(appDir).filePath(QStringLiteral("../share/lincupl/Examples/Atmel")),
        QDir(appDir).filePath(QStringLiteral("share/lincupl/Examples/Atmel")),
        QDir(appDir).filePath(QStringLiteral("../../share/lincupl/Examples/Atmel")),
        QDir::current().filePath(QStringLiteral("share/lincupl/Examples/Atmel")),
        QDir::current().filePath(QStringLiteral("../share/lincupl/Examples/Atmel")),
    });
#ifdef LINSIM_SOURCE_ROOT
    candidates.append(QDir(QStringLiteral(LINSIM_SOURCE_ROOT))
                          .filePath(QStringLiteral("build/share/lincupl/Examples/Atmel")));
    candidates.append(QDir(QStringLiteral(LINSIM_SOURCE_ROOT))
                          .filePath(QStringLiteral("share/lincupl/Examples/Atmel")));
#endif
    for (const QString &cand : candidates) {
        QDir d(cand);
        if (d.exists() && QFileInfo::exists(d.filePath(QStringLiteral("COUNT10.SI")))) {
            return d.canonicalPath();
        }
    }
    return QString();
}

bool copyFileCaseInsensitive(const QString &srcDir, const QString &fileName, const QString &dstDir) {
    QDir sd(srcDir);
    QString actualName;
    for (const QString &entry : sd.entryList(QDir::Files | QDir::Readable)) {
        if (entry.compare(fileName, Qt::CaseInsensitive) == 0) {
            actualName = entry;
            break;
        }
    }
    if (actualName.isEmpty()) return false;
    const QString dstPath = QDir(dstDir).filePath(actualName);
    QFile::remove(dstPath);
    return QFile::copy(sd.filePath(actualName), dstPath);
}

} // namespace

int runLinSimSelfTest() {
    std::fprintf(stdout, "=== LinSim Headless Self-Test ===\n");
    TestStats st;

    QTemporaryDir tempDir;
    st.check(tempDir.isValid(), QStringLiteral("Create isolated temporary test workspace"));
    if (!tempDir.isValid()) {
        return 1;
    }

    // Isolate QSettings inside tempDir so self-test never touches or depends on user settings
    const QString settingsDir = QDir(tempDir.path()).filePath(QStringLiteral("settings"));
    QDir().mkpath(settingsDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    {
        QSettings probe;
        st.check(QFileInfo(probe.fileName()).absoluteFilePath().startsWith(QFileInfo(settingsDir).absoluteFilePath()),
                 QStringLiteral("Isolate QSettings to temporary IniFormat directory"),
                 probe.fileName());
    }

    // 0. Discover sibling CLI tools & device libraries (hard-required, never silently skipped)
    const QString cuplBin = MainWindow::findCliBinary(QStringLiteral("cupl"));
    const QString csimBin = MainWindow::findCliBinary(QStringLiteral("csim"));
    const QStringList libs = MainWindow::discoverLibraryCandidates();
    const QString examplesDir = locateAtmelExamplesDir();

    st.check(!cuplBin.isEmpty(), QStringLiteral("Locate sibling 'cupl' compiler binary"), cuplBin);
    st.check(!csimBin.isEmpty(), QStringLiteral("Locate sibling 'csim' simulator binary"), csimBin);
    st.check(!libs.isEmpty(), QStringLiteral("Locate CUPL device library (.dl)"),
             libs.isEmpty() ? QStringLiteral("none") : libs.first());
    st.check(!examplesDir.isEmpty(), QStringLiteral("Locate Atmel reference .SI/.PLD examples"), examplesDir);

    if (cuplBin.isEmpty() || csimBin.isEmpty() || libs.isEmpty() || examplesDir.isEmpty()) {
        std::fprintf(stderr, "Self-test aborted due to missing required tools/examples.\n");
        return 1;
    }

    // 1. Parser validation across ALL 26 real Atmel .SI files, synthetic edge cases & corrupt inputs
    {
        SimDocument missingDoc;
        QString err;
        st.check(!missingDoc.loadFile(QDir(tempDir.path()).filePath(QStringLiteral("does_not_exist.si")), &err),
                 QStringLiteral("Reject missing file path cleanly"));

        SimDocument binaryDoc;
        st.check(!binaryDoc.loadFile(libs.first(), &err),
                 QStringLiteral("Reject binary .dl file cleanly without crash"));

        // Sweep every single .SI/.si file in share/lincupl/Examples/Atmel/
        QDir exDir(examplesDir);
        const QStringList allSiFiles = exDir.entryList(
            {QStringLiteral("*.si"), QStringLiteral("*.SI")}, QDir::Files | QDir::Readable, QDir::Name);
        int parsedSiCount = 0;
        int roundTripSiCount = 0;
        QString firstSweepErr;
        QString firstRtErr;
        for (const QString &fn : allSiFiles) {
            const QString fullPath = exDir.filePath(fn);
            SimDocument d;
            QString sErr;
            if (d.loadFile(fullPath, &sErr) && !d.signalsList.isEmpty() && !d.vectors.isEmpty()) {
                ++parsedSiCount;

                // Also verify serializeSi -> loadSiText round-trip across ALL 26 files (including GATES.SI & ROULETTE.si)
                SimDocument rtDoc;
                QString rErr;
                const QString serialized = d.serializeSi(fullPath);
                bool rtOk = rtDoc.loadSiText(serialized, fullPath, &rErr) &&
                            rtDoc.allOrderedPins() == d.allOrderedPins() &&
                            rtDoc.vectors.size() == d.vectors.size();
                if (rtOk) {
                    const QStringList pins = d.allOrderedPins();
                    for (int v = 0; v < d.vectors.size() && rtOk; ++v) {
                        for (const QString &pin : pins) {
                            if (rtDoc.pinState(v, pin) != d.pinState(v, pin)) {
                                rtOk = false;
                                rErr = QStringLiteral("Pin '%1' mismatch at vector %2 (%3 vs %4)")
                                           .arg(pin)
                                           .arg(v + 1)
                                           .arg(rtDoc.pinState(v, pin))
                                           .arg(d.pinState(v, pin));
                                break;
                            }
                        }
                    }
                }
                if (rtOk) {
                    ++roundTripSiCount;
                } else if (firstRtErr.isEmpty()) {
                    firstRtErr = QStringLiteral("%1: %2").arg(fn, rErr);
                }
            } else if (firstSweepErr.isEmpty()) {
                firstSweepErr = QStringLiteral("%1: %2").arg(fn, sErr);
            }
        }
        st.check(!allSiFiles.isEmpty() && parsedSiCount == allSiFiles.size(),
                 QStringLiteral("Parse 100%% of Atmel reference .SI files (%1/%2)")
                     .arg(parsedSiCount)
                     .arg(allSiFiles.size()),
                 firstSweepErr);
        st.check(!allSiFiles.isEmpty() && roundTripSiCount == allSiFiles.size(),
                 QStringLiteral("Round-trip serializeSi -> loadSiText on 100%% of Atmel .SI files (%1/%2)")
                     .arg(roundTripSiCount)
                     .arg(allSiFiles.size()),
                 firstRtErr);

        // COUNT10.SI: %N spacing, !oe active-low, Q3..Q0 range bus, 'C' clock, 'Z' tristate, /*...*/ comments
        SimDocument count10;
        const bool okCount10 = count10.loadFile(exDir.filePath(QStringLiteral("COUNT10.SI")), &err);
        st.check(okCount10, QStringLiteral("Parse COUNT10.SI"), err);
        if (okCount10) {
            st.check(count10.signalsList.size() == 6,
                     QStringLiteral("COUNT10.SI has 6 ORDER items (clk, clr, dir, !oe, Q3..Q0, carry)"),
                     QString::number(count10.signalsList.size()));
            st.check(count10.signalsList[3].activeLow &&
                         count10.signalsList[3].name.compare(QStringLiteral("oe"), Qt::CaseInsensitive) == 0,
                     QStringLiteral("COUNT10.SI tracks '!oe' active-low polarity"));
            st.check(count10.signalsList[4].isBus() && count10.signalsList[4].width() == 4,
                     QStringLiteral("COUNT10.SI expands Q3..Q0 into 4-bit RangeBus"));
            bool hasSpacing = false;
            for (const auto &s : count10.signalsList) {
                if (s.spacingAfter > 0) hasSpacing = true;
            }
            st.check(hasSpacing, QStringLiteral("COUNT10.SI preserves %N spacing tokens in ORDER:"));
            st.check(count10.vectors.size() == 24,
                     QStringLiteral("COUNT10.SI parses all 24 test vectors"),
                     QString::number(count10.vectors.size()));

            bool sawClock = false, sawTristate = false, sawComment = false;
            for (int v = 0; v < count10.vectors.size(); ++v) {
                if (count10.pinState(v, QStringLiteral("clk")) == QLatin1Char('C')) sawClock = true;
                if (count10.pinState(v, QStringLiteral("Q0")) == QLatin1Char('Z')) sawTristate = true;
                if (!count10.vectors[v].comment.isEmpty()) sawComment = true;
            }
            st.check(sawClock && sawTristate && sawComment,
                     QStringLiteral("COUNT10.SI parses C clock, Z tristate, and /*...*/ vector comments"));
        }

        // BARREL22.si: companion .PLD FIELD discovery, single-quoted input hex '80', double-quoted output hex "80"
        SimDocument barrel;
        const bool okBarrel = barrel.loadFile(exDir.filePath(QStringLiteral("BARREL22.si")), &err);
        st.check(okBarrel, QStringLiteral("Parse BARREL22.si with companion BARREL22.PLD FIELD metadata"), err);
        if (okBarrel) {
            st.check(barrel.signalsList.size() == 6 &&
                         barrel.signalsList[3].isBus() && barrel.signalsList[3].width() == 3 &&
                         barrel.signalsList[4].isBus() && barrel.signalsList[4].width() == 8 &&
                         barrel.signalsList[5].isBus() && barrel.signalsList[5].width() == 8,
                     QStringLiteral("BARREL22.si resolves PLD FIELDs (shift[3], input[8], output[8])"));
            // Vector 1 is: C00000 '80' "80" /* shift 0 */
            st.check(barrel.vectors.size() >= 2 &&
                         barrel.pinState(1, QStringLiteral("D7")) == QLatin1Char('1') &&
                         barrel.pinState(1, QStringLiteral("D0")) == QLatin1Char('0') &&
                         barrel.pinState(1, QStringLiteral("Q7")) == QLatin1Char('H') &&
                         barrel.pinState(1, QStringLiteral("Q0")) == QLatin1Char('L'),
                     QStringLiteral("BARREL22.si expands '80' -> 10000000 and \"80\" -> HLLLLLLL"));
        }

        // GATES.SI: $MSG directives
        SimDocument gates;
        const bool okGates = gates.loadFile(exDir.filePath(QStringLiteral("GATES.SI")), &err);
        bool hasMsg = !gates.trailingMessages.isEmpty();
        for (const auto &vr : gates.vectors) {
            if (!vr.messagesBefore.isEmpty()) hasMsg = true;
        }
        st.check(okGates && hasMsg, QStringLiteral("Parse GATES.SI including $MSG directives"), err);

        // ADDER.SI: C3..C1 bus with '***' simulator-determined states
        SimDocument adder;
        const bool okAdder = adder.loadFile(exDir.filePath(QStringLiteral("ADDER.SI")), &err);
        st.check(okAdder && !adder.vectors.isEmpty() &&
                     adder.pinState(0, QStringLiteral("C3")) == QLatin1Char('*'),
                 QStringLiteral("Parse ADDER.SI with '*' simulator-determined bus states"), err);

        // CNT10.SI: 985-vector CPLD simulation file
        SimDocument cnt10;
        const bool okCnt10 = cnt10.loadFile(exDir.filePath(QStringLiteral("CNT10.SI")), &err);
        st.check(okCnt10 && cnt10.vectors.size() == 985,
                 QStringLiteral("Parse CNT10.SI large 985-vector CPLD test suite"), err);

        // ROULETTE.si, XORCOM.SI & Join2500.si (multi-ORDER + Ctrl-Z end of file)
        SimDocument roulette, xorcom, join2500;
        st.check(roulette.loadFile(exDir.filePath(QStringLiteral("ROULETTE.si")), &err) &&
                     !roulette.signalsList.isEmpty(),
                 QStringLiteral("Parse ROULETTE.si ascending active-low !L0..L19 range"), err);
        st.check(xorcom.loadFile(exDir.filePath(QStringLiteral("XORCOM.SI")), &err),
                 QStringLiteral("Parse XORCOM.SI multi-line commented ORDER:"), err);
        st.check(join2500.loadFile(exDir.filePath(QStringLiteral("Join2500.si")), &err) &&
                     join2500.vectors.size() == 8,
                 QStringLiteral("Parse Join2500.si multiple ORDER:/VECTORS: sections & Ctrl-Z end of file"), err);

        // Synthetic .SI-local FIELD, BASE: octal, 'P' preload, 'K' falling clock & $REPEAT
        const QString synthSi = QStringLiteral(
            "Name SynthTest;\n"
            "Device g16v8a;\n"
            "BASE: octal;\n"
            "FIELD addr = [A2..0];\n"
            "ORDER: clk, %1, addr, %2, !cs, Q1..Q0;\n"
            "VECTORS:\n"
            "$MSG \"Preload test\";\n"
            "K '5' 0 PP /* preload outputs */\n"
            "$REPEAT 2;\n"
            "C '7' 1 \"3\" /* octal 7 = 111, 3 = HH */\n");
        SimDocument synthDoc;
        const bool okSynth = synthDoc.loadSiText(synthSi, QString(), &err);
        st.check(okSynth &&
                     synthDoc.fileBase == BusRadix::Octal &&
                     synthDoc.fields.contains(QStringLiteral("addr")) &&
                     synthDoc.fields[QStringLiteral("addr")].definedInSi &&
                     synthDoc.vectors.size() == 3 &&
                     synthDoc.pinState(0, QStringLiteral("clk")) == QLatin1Char('K') &&
                     synthDoc.pinState(0, QStringLiteral("Q1")) == QLatin1Char('P') &&
                     synthDoc.pinState(1, QStringLiteral("A2")) == QLatin1Char('1') &&
                     synthDoc.pinState(1, QStringLiteral("Q0")) == QLatin1Char('H') &&
                     synthDoc.serializeSi().contains(QStringLiteral("FIELD addr = [A2,A1,A0];")) &&
                     synthDoc.serializeSi().contains(QStringLiteral("BASE: octal;"), Qt::CaseInsensitive),
                 QStringLiteral("Parse & serialize local FIELD, BASE: octal, K, P, and $REPEAT"), err);
    }

    // 2. End-to-end MainWindow + cupl + csim execution & .WO / .SO loading
    const QString workDir = QDir(tempDir.path()).filePath(QStringLiteral("sim_work"));
    QDir().mkpath(workDir);

    copyFileCaseInsensitive(examplesDir, QStringLiteral("COUNT10.PLD"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("COUNT10.SI"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("BARREL22.PLD"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("BARREL22.si"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("LOOKUP.PLD"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("LOOKUP.SI"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("ADDER.PLD"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("ADDER.SI"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("GATES.PLD"), workDir);
    copyFileCaseInsensitive(examplesDir, QStringLiteral("GATES.SI"), workDir);

    MainWindow win;
    win.resize(1200, 760);

    const QString count10Si = SimDocument::findCompanionFile(
        QDir(workDir).filePath(QStringLiteral("COUNT10.SI")), QStringLiteral(".si"));
    QString err;
    st.check(win.openDocument(count10Si, &err), QStringLiteral("Open COUNT10.SI in MainWindow"), err);

    // Exercise the real menu connections and modal event loops, not only the
    // document/parser APIs. Cancel each dialog without modifying the design.
    for (const QString &label : {
             QStringLiteral("Design Properties..."),
             QStringLiteral("Add Signal / Bus..."),
             QStringLiteral("Add Vectors..."),
             QStringLiteral("Grid Size..."),
             QStringLiteral("Colors..."),
             QStringLiteral("Set Library..."),
             QStringLiteral("Waveform States Shortcuts...")}) {
        QAction *action = nullptr;
        for (QAction *candidate : win.findChildren<QAction *>()) {
            QString text = candidate->text();
            if (text.remove(QLatin1Char('&')).simplified() == label) {
                action = candidate;
                break;
            }
        }
        bool opened = false;
        QTimer closer;
        closer.setSingleShot(true);
        QObject::connect(&closer, &QTimer::timeout, &win, [&]() {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                opened = !dialog->grab().isNull();
                dialog->reject();
            }
        });
        if (action) {
            closer.start(0);
            action->trigger();
            closer.stop();
        }
        st.check(opened, QStringLiteral("Open and dismiss menu dialog: %1").arg(label));
    }

    QString simLog;
    const bool simCount10Ok = win.runSimulation(true, false, &simLog);
    st.check(simCount10Ok, QStringLiteral("Compile & Simulate COUNT10 via cupl + csim -lw"), simLog);

    // A fuse-map run must not require or regenerate the ABS, even with
    // a companion source available. Missing JEDEC is a separate error.
    {
        QAction *mode = win.findChild<QAction *>(QStringLiteral("simulateJedec"));
        const QString abs = SimDocument::findCompanionFile(count10Si, QStringLiteral(".abs"));
        const QString jed = SimDocument::findCompanionFile(count10Si, QStringLiteral(".jed"));
        const bool savedAbs = !abs.isEmpty() && QFile::rename(abs, abs + QStringLiteral(".saved"));
        st.check(mode && savedAbs && !jed.isEmpty(), QStringLiteral("Prepare JEDEC-only simulation"));
        if (mode && savedAbs && !jed.isEmpty()) {
            mode->setChecked(true);
            st.check(win.runSimulation(false, false, &simLog) &&
                         simLog.contains(QStringLiteral("-lwf")) && !QFileInfo::exists(abs),
                     QStringLiteral("JEDEC mode runs csim -f without rebuilding ABS"), simLog);
            const bool savedJed = QFile::rename(jed, jed + QStringLiteral(".saved"));
            st.check(savedJed && !win.runSimulation(false, false, &simLog) &&
                         simLog.contains(QStringLiteral("No companion JEDEC file")),
                     QStringLiteral("JEDEC mode reports a missing fuse map"), simLog);
            if (savedJed) QFile::rename(jed + QStringLiteral(".saved"), jed);
            mode->setChecked(false);
        }
        if (savedAbs) QFile::rename(abs + QStringLiteral(".saved"), abs);
    }

    const QString count10Wo = SimDocument::findCompanionFile(count10Si, QStringLiteral(".wo"));
    st.check(!count10Wo.isEmpty() && QFileInfo::exists(count10Wo),
             QStringLiteral("COUNT10.wo generated on disk"));
    st.check(win.soListingText().contains(QStringLiteral("CSIM"), Qt::CaseInsensitive) &&
                 win.soListingText().contains(QStringLiteral("0001:")),
             QStringLiteral(".SO Simulation Listing tab populated after simulation"));
    st.check(win.siSourceText().contains(QStringLiteral("ORDER:"), Qt::CaseInsensitive) &&
                 win.siSourceText().contains(QStringLiteral("VECTORS:"), Qt::CaseInsensitive),
             QStringLiteral(".SI Simulation Source tab synchronized"));

    // Direct .WO file load verification (#WAVEFORM, #H, #F, #V)
    {
        SimDocument woDoc;
        const bool woLoaded = woDoc.loadFile(count10Wo, &err);
        st.check(woLoaded && woDoc.loadedFromWo &&
                     woDoc.signalsList.size() == win.document().signalsList.size() &&
                     woDoc.vectors.size() == win.document().vectors.size(),
                 QStringLiteral("Direct .WO (#WAVEFORM/#H/#F/#V) parser matches .SI structure"), err);
    }

    // Simulate BARREL22.si and verify round-trip save does NOT emit duplicate PLD FIELDs
    const QString barrelSi = SimDocument::findCompanionFile(
        QDir(workDir).filePath(QStringLiteral("BARREL22.si")), QStringLiteral(".si"));
    st.check(win.openDocument(barrelSi, &err), QStringLiteral("Open BARREL22.si in MainWindow"), err);
    st.check(win.runSimulation(true, false, &simLog),
             QStringLiteral("Compile & Simulate BARREL22.si via cupl + csim -lw"), simLog);
    st.check(win.document().signalsList.size() == 6 &&
                 win.document().signalsList[3].isBus() &&
                 win.document().signalsList[4].isBus() &&
                 win.document().signalsList[5].isBus(),
             QStringLiteral("BARREL22 bus groupings (shift, input, output) preserved across .wo reload"));

    // Save BARREL22.si round-trip and re-simulate (verifies no [0015sa] duplicate FIELD error!)
    win.document().modified = true;
    st.check(win.saveDocument(&err), QStringLiteral("Round-trip save BARREL22.si"), err);
    st.check(win.runSimulation(false, false, &simLog),
             QStringLiteral("Re-simulate round-trip saved BARREL22.si cleanly (no duplicate FIELD)"), simLog);

    // Simulate LOOKUP.SI and ADDER.SI (where ADDER C3..C1 outputs in .SI are '***' simulator-determined)
    const QString lookupSi = SimDocument::findCompanionFile(
        QDir(workDir).filePath(QStringLiteral("LOOKUP.SI")), QStringLiteral(".si"));
    st.check(win.openDocument(lookupSi, &err) && win.runSimulation(true, false, &simLog),
             QStringLiteral("Compile & Simulate LOOKUP.SI cleanly"), simLog);

    const QString adderSi = SimDocument::findCompanionFile(
        QDir(workDir).filePath(QStringLiteral("ADDER.SI")), QStringLiteral(".si"));
    st.check(win.openDocument(adderSi, &err), QStringLiteral("Open ADDER.SI in MainWindow"), err);
    st.check(win.runSimulation(true, false, &simLog),
             QStringLiteral("Compile & Simulate ADDER.SI ('***' outputs computed by csim)"), simLog);
    {
        const QChar c3Computed = win.document().pinState(0, QStringLiteral("C3"));
        st.check(c3Computed == QLatin1Char('L') || c3Computed == QLatin1Char('H'),
                 QStringLiteral("ADDER.SI '*' output C3 updated to concrete L/H from simulated .wo"),
                 QString(c3Computed));
    }

    // Round-trip save GATES.SI (contains "Order:" and "Vectors:" inside /* ... */ comments) and simulate via cupl+csim
    const QString gatesSi = SimDocument::findCompanionFile(
        QDir(workDir).filePath(QStringLiteral("GATES.SI")), QStringLiteral(".si"));
    st.check(win.openDocument(gatesSi, &err), QStringLiteral("Open GATES.SI in MainWindow"), err);
    const int gatesVecCount = win.document().vectors.size();
    win.document().modified = true;
    st.check(gatesVecCount == 9 && win.saveDocument(&err) && win.openDocument(gatesSi, &err) &&
                 win.document().vectors.size() == gatesVecCount,
             QStringLiteral("Round-trip save & reload GATES.SI preserves all 9 vectors"), err);
    st.check(win.runSimulation(true, false, &simLog) && win.document().vectors.size() == gatesVecCount,
             QStringLiteral("Compile & Simulate round-trip saved GATES.SI (9 vectors)"), simLog);

    // 3. Expected-output mismatch (#E) detection & Assign Simulator Results
    st.check(win.openDocument(count10Si, &err), QStringLiteral("Re-open COUNT10.SI for mismatch & editing tests"), err);
    {
        // Vector 0 has clr=1 -> carry is actually L; set expected carry='H' to trigger [0019sa] / #E
        win.document().setPinState(0, QStringLiteral("carry"), QLatin1Char('H'));
        const bool mismatchRun = win.runSimulation(false, false, &simLog);
        const SimError *simErr = win.document().findError(0, QStringLiteral("carry"));
        st.check(!mismatchRun && simErr != nullptr && simErr->expected == QLatin1Char('H'),
                 QStringLiteral("Detect CSIM [0019sa] mismatch and parse #E line on 'carry'"), simLog);

        // Re-simulating BEFORE Assign Simulator Results must STILL fail (must NOT overwrite expected 'H' in .SI)
        const bool secondMismatchRun = win.runSimulation(false, false, &simLog);
        const SimError *simErr2 = win.document().findError(0, QStringLiteral("carry"));
        st.check(!secondMismatchRun && simErr2 != nullptr && simErr2->expected == QLatin1Char('H'),
                 QStringLiteral("Re-simulating before Assign Simulator Results preserves expected 'H' & fails"),
                 simLog);

        // Assign simulator results clears #E and adopts simulated L/H outputs; re-simulation now passes!
        win.document().assignSimulatorResults();
        st.check(win.document().findError(0, QStringLiteral("carry")) == nullptr &&
                     win.document().pinState(0, QStringLiteral("carry")) == QLatin1Char('L'),
                 QStringLiteral("Assign Simulator Results resolves mismatch to actual 'L'"));
        st.check(win.runSimulation(false, false, &simLog),
                 QStringLiteral("Re-simulating after Assign Simulator Results passes cleanly"), simLog);
    }

    // 4. Interactive Waveform Editing + Round-Trip .SI Save + Re-Simulation
    {
        SimDocument &doc = win.document();
        WaveformEditorWidget *ed = win.waveformEditor();

        // Find Q3..Q0 bus index
        int qBusIdx = -1;
        for (int i = 0; i < doc.signalsList.size(); ++i) {
            if (doc.signalsList[i].isBus()) {
                qBusIdx = i;
                break;
            }
        }
        st.check(qBusIdx >= 0, QStringLiteral("Locate Q3..Q0 bus in COUNT10"));

        // Set bus value in Hex/Bin, test overflow rejection & leading-zero padding, and test formatting
        if (qBusIdx >= 0) {
            QString ovfErr;
            st.check(!doc.setBusValueFromString(qBusIdx, 1, 1, QStringLiteral("1F"), BusRadix::Hex, true, &ovfErr) &&
                         !doc.setBusValueFromString(qBusIdx, 1, 1, QStringLiteral("99"), BusRadix::Decimal, true, &ovfErr) &&
                         !doc.setBusValueFromString(qBusIdx, 1, 1, QStringLiteral("16"), BusRadix::Decimal, true, &ovfErr),
                     QStringLiteral("SetBusValue rejects non-zero overflow ('1F' hex, '99'/'16' dec) on 4-bit bus"));
            st.check(doc.setBusValueFromString(qBusIdx, 1, 1, QStringLiteral("015"), BusRadix::Decimal, true, &err) &&
                         doc.setBusValueFromString(qBusIdx, 1, 1, QStringLiteral("0A"), BusRadix::Hex, true, &err),
                     QStringLiteral("SetBusValue accepts leading-zero padded values ('015' dec, '0A' hex) on 4-bit bus"), err);
            st.check(doc.formatBusValue(doc.signalsList[qBusIdx], 1, BusRadix::Hex, false) == QStringLiteral("A") &&
                         doc.formatBusValue(doc.signalsList[qBusIdx], 1, BusRadix::Decimal, false) == QStringLiteral("10") &&
                         doc.formatBusValue(doc.signalsList[qBusIdx], 1, BusRadix::Octal, false) == QStringLiteral("12") &&
                         doc.formatBusValue(doc.signalsList[qBusIdx], 1, BusRadix::Binary, false) == QStringLiteral("1010"),
                     QStringLiteral("Format bus value accurately across Hex / Dec / Oct / Bin"));

            // Set outputs on vectors 1..4 to '*' (simulator-determined) so CSIM computes them!
            for (int v = 1; v <= 4 && v < doc.vectors.size(); ++v) {
                for (const QString &m : doc.signalsList[qBusIdx].members) {
                    doc.setPinState(v, m, QLatin1Char('*'));
                }
                doc.setPinState(v, QStringLiteral("carry"), QLatin1Char('*'));
            }
        }

        // Vector insert, duplicate, delete
        const int origVecCount = doc.vectors.size();
        doc.duplicateVector(origVecCount - 1);
        doc.insertVector(origVecCount + 1, 1, QLatin1Char('0'), QLatin1Char('*'), false);
        st.check(doc.vectors.size() == origVecCount + 2,
                 QStringLiteral("Duplicate & insert test vectors"));
        doc.deleteVectors(origVecCount, origVecCount + 1);
        st.check(doc.vectors.size() == origVecCount,
                 QStringLiteral("Delete test vectors restores vector count"));

        // Signal add, rename, move, delete
        const int origSigCount = doc.signalsList.size();
        SignalItem auxSig;
        auxSig.kind = SignalItem::Kind::Scalar;
        auxSig.name = QStringLiteral("aux_in");
        auxSig.members = {QStringLiteral("aux_in")};
        st.check(doc.addSignal(auxSig, -1, QLatin1Char('0'), &err) &&
                     doc.renameSignal(origSigCount, QStringLiteral("!aux_ren"), false, &err),
                 QStringLiteral("Add and rename signal with '!' active-low prefix"), err);
        doc.moveSignal(origSigCount, 0);
        st.check(doc.signalsList[0].name == QStringLiteral("aux_ren") && doc.signalsList[0].activeLow,
                 QStringLiteral("Reorder signal in ORDER: list"));
        doc.deleteSignal(0);
        st.check(doc.signalsList.size() == origSigCount,
                 QStringLiteral("Delete temporary signal cleanly"));

        ed->rebuildRows();

        // Save edited COUNT10.SI, reload into fresh SimDocument to verify round-trip, then re-run CSIM!
        st.check(win.saveDocument(&err), QStringLiteral("Save edited COUNT10.SI to disk"), err);

        SimDocument reloaded;
        st.check(reloaded.loadFile(count10Si, &err) &&
                     reloaded.vectors.size() == origVecCount &&
                     reloaded.pinState(2, QStringLiteral("Q3")) == QLatin1Char('*'),
                 QStringLiteral("Reload saved COUNT10.SI preserves '*' edits & vector count"), err);

        st.check(win.runSimulation(false, false, &simLog),
                 QStringLiteral("Re-simulate round-trip edited COUNT10.SI via csim (exit 0)"), simLog);
        st.check(win.document().pinState(2, QStringLiteral("Q3")) == QLatin1Char('L') ||
                     win.document().pinState(2, QStringLiteral("Q3")) == QLatin1Char('H'),
                 QStringLiteral("CSIM computes concrete L/H outputs for edited '*' vectors"));
    }

    // 5. Offscreen Waveform Rendering (QImage) & PNG / PDF Export
    {
        WaveformEditorWidget *ed = win.waveformEditor();
        ed->setAllBusesExpanded(true);
        ed->zoomIn();
        ed->setWideSignals(true);
        ed->setShowGrid(true);

        // Exercise falling clock 'K', preload 'P', and don't-care 'X' in offscreen paint
        if (win.document().vectors.size() >= 4) {
            win.document().setPinState(1, QStringLiteral("clk"), QLatin1Char('K'));
            win.document().setPinState(2, QStringLiteral("dir"), QLatin1Char('X'));
            win.document().setPinState(3, QStringLiteral("Q0"), QLatin1Char('P'));
        }
        ed->refreshAll();

        const QImage img = ed->renderToImage(24);
        st.check(!img.isNull() && img.width() > 300 && img.height() > 150,
                 QStringLiteral("Render waveform widget to offscreen QImage"),
                 QStringLiteral("%1x%2").arg(img.width()).arg(img.height()));

        QSet<QRgb> uniqueColors;
        if (!img.isNull()) {
            const int stepX = qMax(1, img.width() / 80);
            const int stepY = qMax(1, img.height() / 60);
            for (int y = 0; y < img.height(); y += stepY) {
                for (int x = 0; x < img.width(); x += stepX) {
                    uniqueColors.insert(img.pixel(x, y));
                }
            }
        }
        st.check(uniqueColors.size() >= 6,
                 QStringLiteral("Offscreen waveform image contains multi-color traces, headers & grid"),
                 QStringLiteral("%1 distinct sampled colors").arg(uniqueColors.size()));

        const QString pngPath = QDir(tempDir.path()).filePath(QStringLiteral("count10_wave.png"));
        const bool pngOk = ed->exportPng(pngPath, &err);
        QImage reloadedPng(pngPath);
        st.check(pngOk && QFileInfo(pngPath).size() > 1000 && !reloadedPng.isNull(),
                 QStringLiteral("Export waveform to valid PNG image"), err);

        const QString pdfPath = QDir(tempDir.path()).filePath(QStringLiteral("count10_wave.pdf"));
        const bool pdfOk = ed->exportPdf(pdfPath, &err);
        QFile pdfFile(pdfPath);
        QByteArray pdfMagic;
        if (pdfFile.open(QIODevice::ReadOnly)) {
            pdfMagic = pdfFile.read(5);
        }
        st.check(pdfOk && QFileInfo(pdfPath).size() > 500 && pdfMagic == QByteArray("%PDF-"),
                 QStringLiteral("Export waveform to valid PDF document"), err);
    }

    // 6. Find: signal names, bus members, vector numbers, comments and text tabs
    {
        const QString findDir = QDir(tempDir.path()).filePath(QStringLiteral("find_work"));
        QDir().mkpath(findDir);
        copyFileCaseInsensitive(examplesDir, QStringLiteral("COUNT10.SI"), findDir);
        const QString findSi = SimDocument::findCompanionFile(
            QDir(findDir).filePath(QStringLiteral("COUNT10.SI")), QStringLiteral(".si"));
        st.check(win.openDocument(findSi, &err), QStringLiteral("Open pristine COUNT10.SI for Find"), err);
        win.viewTabs()->setCurrentIndex(0);
        WaveformEditorWidget *ed = win.waveformEditor();
        const SimDocument &doc = win.document();
        using Target = FindDialog::Target;

        auto selectedRow = [ed]() {
            int r0, r1;
            ed->selectedRowRange(r0, r1);
            return ed->visibleRows().value(r1);
        };
        auto selectedName = [&]() {
            const VisibleWaveformRow row = selectedRow();
            if (row.sigIndex < 0) return QString();
            const SignalItem &sig = doc.signalsList[row.sigIndex];
            return row.isBusMember() ? sig.members[row.memberIndex] : sig.name;
        };

        st.check(win.find(QStringLiteral("carry"), Target::SignalName, false, false, false) &&
                     selectedName() == QStringLiteral("carry"),
                 QStringLiteral("Find signal 'carry' selects its row"), selectedName());
        st.check(win.find(QStringLiteral("CL"), Target::SignalName, false, false, false) &&
                     selectedName() == QStringLiteral("clk") &&
                     win.find(QStringLiteral("CL"), Target::SignalName, false, false, false) &&
                     selectedName() == QStringLiteral("clr") &&
                     win.find(QStringLiteral("CL"), Target::SignalName, false, false, true) &&
                     selectedName() == QStringLiteral("clk"),
                 QStringLiteral("Find Next / Previous wrap through matching signals (clk, clr, clk)"),
                 selectedName());
        st.check(!win.find(QStringLiteral("CL"), Target::SignalName, true, false, false),
                 QStringLiteral("Find with Match case rejects 'CL' for 'clk'"));
        st.check(win.find(QStringLiteral("Q1"), Target::SignalName, false, true, false) &&
                     selectedName() == QStringLiteral("Q1") && selectedRow().isBusMember(),
                 QStringLiteral("Find bus member 'Q1' expands the bus and selects the member row"),
                 selectedName());
        st.check(!win.find(QStringLiteral("nosuchsignal"), Target::SignalName, false, false, false),
                 QStringLiteral("Find reports a missing signal name"));

        st.check(win.find(QStringLiteral("7"), Target::VectorNumber, false, false, false) &&
                     ed->cursorVector() == 6,
                 QStringLiteral("Find vector number 7 moves the cursor to vector 7"),
                 QString::number(ed->cursorVector() + 1));
        st.check(!win.find(QStringLiteral("999"), Target::VectorNumber, false, false, false),
                 QStringLiteral("Find rejects a vector number past the end"));

        ed->selectVector(0);
        const bool carry1 = win.find(QStringLiteral("carry"), Target::VectorText, false, false, false) &&
                            ed->cursorVector() == 9;
        const bool carry2 = win.find(QStringLiteral("carry"), Target::VectorText, false, false, false) &&
                            ed->cursorVector() == 20;
        const bool carry3 = win.find(QStringLiteral("carry"), Target::VectorText, false, false, false) &&
                            ed->cursorVector() == 9;
        const bool carryBack = win.find(QStringLiteral("carry"), Target::VectorText, false, false, true) &&
                               ed->cursorVector() == 20;
        st.check(carry1 && carry2 && carry3 && carryBack,
                 QStringLiteral("Find vector comment 'carry' steps through vectors 10, 21 and wraps"),
                 QString::number(ed->cursorVector() + 1));

        win.viewTabs()->setCurrentIndex(2);
        QPlainTextEdit *siEd = win.siSourceEditor();
        siEd->moveCursor(QTextCursor::Start);
        st.check(win.find(QStringLiteral("vectors:"), Target::SignalName, false, false, false) &&
                     siEd->textCursor().selectedText().compare(QStringLiteral("VECTORS:"), Qt::CaseInsensitive) == 0,
                 QStringLiteral("Find in the .SI tab selects the text in the editor"),
                 siEd->textCursor().selectedText());
        st.check(!win.find(QStringLiteral("nosuchtext"), Target::SignalName, false, false, false),
                 QStringLiteral("Find in the .SI tab reports missing text"));

        // The menu path: Find Next without a search opens the Find box, then
        // Find Next searches with its text.
        win.viewTabs()->setCurrentIndex(0);
        QAction *findNextAct = nullptr;
        for (QAction *a : win.findChildren<QAction *>()) {
            if (a->text().remove(QLatin1Char('&')) == QStringLiteral("Find Next")) findNextAct = a;
        }
        st.check(findNextAct != nullptr && findNextAct->shortcuts().contains(QKeySequence(Qt::Key_F3)),
                 QStringLiteral("Edit menu has Find Next on F3"));
        if (findNextAct) {
            findNextAct->trigger();
            FindDialog *dlg = win.findDialog();
            st.check(dlg->isVisible(), QStringLiteral("Find Next without a search opens the Find box"));
            dlg->setText(QStringLiteral("dir"));
            dlg->setTarget(Target::SignalName);
            findNextAct->trigger();
            st.check(selectedName() == QStringLiteral("dir"),
                     QStringLiteral("Find Next from the menu finds the Find box text"), selectedName());
            dlg->close();
        }
    }

    // 7. Print Colors: colour and black-and-white rendering for Print / PDF
    {
        WaveformEditorWidget *ed = win.waveformEditor();
        SimDocument &doc = win.document();
        ed->setAllBusesExpanded(true);
        if (doc.vectors.size() >= 6) {
            doc.setPinState(1, QStringLiteral("clk"), QLatin1Char('K'));
            doc.setPinState(2, QStringLiteral("dir"), QLatin1Char('X'));
            doc.setPinState(3, QStringLiteral("Q0"), QLatin1Char('P'));
            doc.setPinState(4, QStringLiteral("Q1"), QLatin1Char('*'));
            SimError mismatch;
            mismatch.expected = QLatin1Char('H');
            mismatch.pinName = QStringLiteral("carry");
            doc.vectors[5].errors.append(mismatch);
        }
        doc.signalsList[0].customColor = QColor(255, 0, 0);
        ed->refreshAll();

        const QImage colorImg = ed->renderToImage(24, false);
        const QImage monoImg = ed->renderToImage(24, true);
        auto isGray = [](QRgb px) {
            return qAbs(qRed(px) - qGreen(px)) <= 6 && qAbs(qGreen(px) - qBlue(px)) <= 6;
        };
        int colorPixels = 0;
        for (int y = 0; y < colorImg.height(); ++y) {
            for (int x = 0; x < colorImg.width(); ++x) {
                if (!isGray(colorImg.pixel(x, y))) ++colorPixels;
            }
        }
        int monoColorPixels = 0, darkPixels = 0, whitePixels = 0;
        QSet<int> grays;
        for (int y = 0; y < monoImg.height(); ++y) {
            for (int x = 0; x < monoImg.width(); ++x) {
                const QRgb px = monoImg.pixel(x, y);
                if (!isGray(px)) ++monoColorPixels;
                if (qGray(px) < 64) ++darkPixels;
                if (qGray(px) == 255) ++whitePixels;
                grays.insert(qGray(px) / 16);
            }
        }
        st.check(colorPixels > 1000, QStringLiteral("Colour print image uses colours"),
                 QStringLiteral("%1 coloured pixels").arg(colorPixels));
        st.check(monoImg.size() == colorImg.size() && monoColorPixels == 0,
                 QStringLiteral("Black-and-white print image has no colour, also for a custom signal colour"),
                 QStringLiteral("%1 coloured pixels").arg(monoColorPixels));
        st.check(darkPixels > 1000 && whitePixels > monoImg.width() * monoImg.height() / 3 && grays.size() >= 4,
                 QStringLiteral("Black-and-white print image draws dark traces on white with grey patterns"),
                 QStringLiteral("%1 dark, %2 white, %3 grey levels").arg(darkPixels).arg(whitePixels).arg(grays.size()));
        st.check(ed->renderToImage(24, false) == colorImg,
                 QStringLiteral("Colour rendering is unchanged after a black-and-white render"));

        const QString monoPdf = QDir(tempDir.path()).filePath(QStringLiteral("count10_bw.pdf"));
        QFile::remove(monoPdf);
        st.check(ed->exportPdf(monoPdf, &err, false) && QFileInfo(monoPdf).size() > 500,
                 QStringLiteral("Export waveform to black-and-white PDF"), err);

        st.check(win.printColors(), QStringLiteral("Print Colors is on by default"));
        win.setPrintColors(false);
        {
            MainWindow other;
            st.check(!other.printColors(), QStringLiteral("Print Colors setting persists in QSettings"));
        }
        win.setPrintColors(true);
        doc.signalsList[0].customColor = QColor();
    }

    // 8. Several documents: one top-level window per document
    {
        auto actionIn = [](QWidget *w, const QString &label) -> QAction * {
            for (QAction *a : w->findChildren<QAction *>()) {
                if (a->text().remove(QLatin1Char('&')) == label) return a;
            }
            return nullptr;
        };
        st.check(MainWindow::windows() == QList<MainWindow *>{&win},
                 QStringLiteral("The self-test window is the only LinSim window"),
                 QString::number(MainWindow::windows().size()));

        const QString multiDir = QDir(tempDir.path()).filePath(QStringLiteral("multi_work"));
        QDir().mkpath(multiDir);
        copyFileCaseInsensitive(examplesDir, QStringLiteral("GATES.SI"), multiDir);
        copyFileCaseInsensitive(examplesDir, QStringLiteral("ADDER.SI"), multiDir);
        const QString gatesPath = SimDocument::findCompanionFile(
            QDir(multiDir).filePath(QStringLiteral("GATES.SI")), QStringLiteral(".si"));
        const QString adderPath = SimDocument::findCompanionFile(
            QDir(multiDir).filePath(QStringLiteral("ADDER.SI")), QStringLiteral(".si"));

        // win holds COUNT10.SI, so GATES.SI opens in a second window.
        const QString winPath = win.document().filePath;
        QPointer<MainWindow> second = MainWindow::openInWindow(gatesPath, &win, &err);
        st.check(second && second != &win && MainWindow::windows().size() == 2 &&
                     second->isVisible() && second->testAttribute(Qt::WA_DeleteOnClose) &&
                     second->document().filePath == gatesPath && win.document().filePath == winPath,
                 QStringLiteral("Opening a file from a window with a document opens a second window"), err);
        st.check(MainWindow::openInWindow(gatesPath, &win, &err) == second &&
                     MainWindow::windows().size() == 2,
                 QStringLiteral("Opening a file that is already open switches to its window"));

        // An untitled, unchanged window is reused.
        QPointer<MainWindow> empty = MainWindow::newWindow(&win);
        st.check(empty && empty->isUntouched() &&
                     MainWindow::openInWindow(adderPath, empty, &err) == empty &&
                     MainWindow::windows().size() == 3,
                 QStringLiteral("Opening a file from an empty window uses that window"), err);

        if (second) {
            // Edits and undo stay in their own window.
            QAction *winUndo = actionIn(&win, QStringLiteral("Undo"));
            QAction *secondUndo = actionIn(second, QStringLiteral("Undo"));
            const bool winUndoBefore = winUndo && winUndo->isEnabled();
            const QChar winState = win.document().pinState(0, win.document().allOrderedPins().first());
            second->waveformEditor()->selectAll();
            second->waveformEditor()->applyStateToSelection(QLatin1Char('X'));
            st.check(secondUndo && secondUndo->isEnabled() && winUndo &&
                         winUndo->isEnabled() == winUndoBefore && second->document().modified &&
                         win.document().pinState(0, win.document().allOrderedPins().first()) == winState,
                     QStringLiteral("Edits and undo of one window leave the other window alone"));
            if (secondUndo) secondUndo->trigger();
            st.check(!second->document().modified,
                     QStringLiteral("Undo in the second window restores its own document"));

            // Window menu lists all windows and marks the current one.
            QMenu *winMenu = nullptr;
            for (QAction *a : win.menuBar()->actions()) {
                if (a->text().remove(QLatin1Char('&')) == QStringLiteral("Window")) winMenu = a->menu();
            }
            QStringList listed;
            QString checked;
            if (winMenu) {
                emit winMenu->aboutToShow();
                for (QAction *a : winMenu->actions()) {
                    QString text = a->text().remove(QLatin1Char('&'));
                    if (text.endsWith(QStringLiteral(" *"))) text.chop(2);
                    if (text.endsWith(QStringLiteral(".SI"), Qt::CaseInsensitive)) {
                        listed.append(text);
                        if (a->isChecked()) checked = text;
                    }
                }
            }
            st.check(listed.size() == 3 && listed[1].endsWith(QFileInfo(gatesPath).fileName()) &&
                         checked == listed[0],
                     QStringLiteral("Window menu lists every LinSim window and checks the current one"),
                     listed.join(QStringLiteral(", ")));

            // Shared settings reach the other windows.
            win.setPrintColors(false);
            st.check(!second->printColors() && !empty->printColors(),
                     QStringLiteral("Print Colors changed in one window applies to all windows"));
            win.setPrintColors(true);

            // Cascade and Tile on the screen's available area.
            win.show();
            QScreen *scr = win.screen();
            const QRect area = scr->availableGeometry();
            MainWindow::arrangeWindows(true, scr);
            const QRect g1 = win.frameGeometry(), g2 = second->frameGeometry(), g3 = empty->frameGeometry();
            st.check(MainWindow::canArrangeWindows() && g1.topLeft() == area.topLeft() &&
                         g1.topLeft() != g2.topLeft() && g2.topLeft() != g3.topLeft() &&
                         area.contains(g2.topLeft()) && area.contains(g3.topLeft()),
                     QStringLiteral("Tile places the windows side by side on the available screen area"),
                     QStringLiteral("%1,%2 %3,%4 %5,%6").arg(g1.x()).arg(g1.y()).arg(g2.x()).arg(g2.y()).arg(g3.x()).arg(g3.y()));
            MainWindow::arrangeWindows(false, scr);
            QList<QPoint> corners = {win.frameGeometry().topLeft(), second->frameGeometry().topLeft(),
                                     empty->frameGeometry().topLeft()};
            std::sort(corners.begin(), corners.end(),
                      [](const QPoint &a, const QPoint &b) { return a.x() < b.x(); });
            const QPoint d1 = corners[1] - corners[0];
            const QPoint d2 = corners[2] - corners[1];
            st.check(corners[0] == area.topLeft() && d1.x() > 0 && d1 == QPoint(d1.x(), d1.x()) &&
                         d2 == d1 && area.contains(corners[2]),
                     QStringLiteral("Cascade offsets each window from the previous one"),
                     QStringLiteral("%1,%2").arg(d1.x()).arg(d1.y()));
            win.hide();

            // Closing a window removes it from the list; it deletes itself.
            empty->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            st.check(!empty && MainWindow::windows().size() == 2,
                     QStringLiteral("Closing a window deletes it and drops it from the window list"));
            second->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    // 9. Save All: every modified window's document is saved
    {
        const QString saveDir = QDir(tempDir.path()).filePath(QStringLiteral("saveall_work"));
        QDir().mkpath(saveDir);
        copyFileCaseInsensitive(examplesDir, QStringLiteral("COUNT10.SI"), saveDir);
        copyFileCaseInsensitive(examplesDir, QStringLiteral("GATES.SI"), saveDir);
        copyFileCaseInsensitive(examplesDir, QStringLiteral("ADDER.SI"), saveDir);
        const QString countPath = SimDocument::findCompanionFile(
            QDir(saveDir).filePath(QStringLiteral("COUNT10.SI")), QStringLiteral(".si"));
        const QString gatesPath = SimDocument::findCompanionFile(
            QDir(saveDir).filePath(QStringLiteral("GATES.SI")), QStringLiteral(".si"));
        const QString adderPath = SimDocument::findCompanionFile(
            QDir(saveDir).filePath(QStringLiteral("ADDER.SI")), QStringLiteral(".si"));

        st.check(win.openDocument(countPath, &err), QStringLiteral("Open COUNT10.SI for Save All"), err);
        QPointer<MainWindow> gatesWin = MainWindow::openInWindow(gatesPath, &win, &err);
        QPointer<MainWindow> adderWin = MainWindow::openInWindow(adderPath, &win, &err);
        st.check(gatesWin && adderWin && MainWindow::windows().size() == 3,
                 QStringLiteral("Open GATES.SI and ADDER.SI in two more windows"), err);

        QAction *saveAllAct = nullptr;
        for (QAction *a : win.findChildren<QAction *>()) {
            if (a->text().remove(QLatin1Char('&')) == QStringLiteral("Save All")) saveAllAct = a;
        }
        st.check(saveAllAct && saveAllAct->shortcut() == QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S),
                 QStringLiteral("File menu has Save All on Ctrl+Alt+S"));

        if (gatesWin && adderWin) {
            const QString countPin = win.document().allOrderedPins().first();
            const QString gatesPin = gatesWin->document().allOrderedPins().first();
            const QChar countNew = win.document().pinState(0, countPin) == QLatin1Char('X') ? QLatin1Char('0') : QLatin1Char('X');
            const QChar gatesNew = gatesWin->document().pinState(0, gatesPin) == QLatin1Char('X') ? QLatin1Char('0') : QLatin1Char('X');
            win.document().setPinState(0, countPin, countNew);
            win.document().modified = true;
            gatesWin->document().setPinState(0, gatesPin, gatesNew);
            gatesWin->document().modified = true;
            const QDateTime adderTime = QFileInfo(adderPath).lastModified();

            st.check(MainWindow::saveAll(false, &err), QStringLiteral("Save All saves every modified window"), err);
            SimDocument countDisk, gatesDisk;
            st.check(!win.hasUnsavedChanges() && !gatesWin->hasUnsavedChanges() &&
                         countDisk.loadFile(countPath, &err) && countDisk.pinState(0, countPin) == countNew &&
                         gatesDisk.loadFile(gatesPath, &err) && gatesDisk.pinState(0, gatesPin) == gatesNew,
                     QStringLiteral("Save All wrote both changed documents to disk"), err);
            st.check(QFileInfo(adderPath).lastModified() == adderTime,
                     QStringLiteral("Save All leaves an unchanged document's file alone"));

            // An untitled document cannot be saved without asking for a name.
            QPointer<MainWindow> untitled = MainWindow::newWindow(&win);
            untitled->document().modified = true;
            st.check(!MainWindow::saveAll(false, &err) && !err.isEmpty(),
                     QStringLiteral("Save All without dialogs reports an untitled document"), err);
            untitled->document().modified = false;

            for (const QPointer<MainWindow> &w : {gatesWin, adderWin, untitled}) {
                if (w) w->close();
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            st.check(MainWindow::windows().size() == 1,
                     QStringLiteral("Close the Save All windows"));
        }
    }

    std::fprintf(stdout, "=== Summary: %d passed, %d failed ===\n", st.passed, st.failed);
    return (st.failed == 0) ? 0 : 1;
}
