#include "selftest.h"

#include "../common/cupl_highlighter.h"
#include "../common/help_window.h"
#include "code_editor.h"
#include "compiler_options.h"
#include "device_library.h"
#include "dialogs.h"
#include "error_catalog.h"
#include "font_settings.h"
#include "mainwindow.h"
#include "tool_paths.h"
#include "user_tools.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QRadioButton>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QMap>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <cstdio>

namespace SelfTest {

namespace {

bool check(bool cond, const char *desc, const QString &detail = QString()) {
    if (cond) {
        std::fprintf(stdout, "[PASS] %s\n", desc);
        std::fflush(stdout);
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s%s%s\n",
                 desc,
                 detail.isEmpty() ? "" : ": ",
                 detail.toUtf8().constData());
    std::fflush(stderr);
    return false;
}

// One token of a highlighting sample: the n-th occurrence of `token` on
// line `line` (0-based) is expected in highlight class `cls`; "plain" means
// no colour at all.
struct TokenClass {
    int line;
    const char *token;
    int occurrence;
    const char *cls;
};

// Highlight `text` as `language` and check that every token has one
// format throughout, that tokens of one class share a colour, that "plain"
// tokens have none and, if `distinct`, that different classes differ.
bool checkHighlighting(const char *desc, const QString &text, CuplHighlighter::Language language,
                       const QList<TokenClass> &expect, bool distinct) {
    QTextDocument doc;
    doc.setPlainText(text);
    CuplHighlighter highlighter(&doc, language, CuplHighlighter::Theme::Light);
    highlighter.rehighlight();

    auto colourAt = [](const QTextBlock &blk, int pos) -> QString {
        for (const QTextLayout::FormatRange &r : blk.layout()->formats()) {
            if (pos >= r.start && pos < r.start + r.length) {
                const QBrush b = r.format.foreground();
                return b.style() == Qt::NoBrush ? QString() : b.color().name();
            }
        }
        return QString();
    };

    QMap<QString, QString> colourOf;
    QStringList problems;
    for (const TokenClass &t : expect) {
        const QTextBlock blk = doc.findBlockByNumber(t.line);
        int pos = -1;
        for (int i = 0; i <= t.occurrence; ++i) {
            pos = blk.text().indexOf(QLatin1String(t.token), pos + 1);
        }
        const QString where = QStringLiteral("line %1 '%2'").arg(t.line).arg(QLatin1String(t.token));
        if (pos < 0) {
            problems << where + QStringLiteral(" not found");
            continue;
        }
        const QString colour = colourAt(blk, pos);
        for (int i = 1; i < int(qstrlen(t.token)); ++i) {
            if (colourAt(blk, pos + i) != colour) {
                problems << where + QStringLiteral(" is split");
                break;
            }
        }
        const QString cls = QLatin1String(t.cls);
        if (cls == QLatin1String("plain")) {
            if (!colour.isEmpty()) {
                problems << where + QStringLiteral(" should be plain, is ") + colour;
            }
        } else if (colour.isEmpty()) {
            problems << where + QStringLiteral(" should be ") + cls + QStringLiteral(", is plain");
        } else if (colourOf.contains(cls) && colourOf[cls] != colour) {
            problems << where + QStringLiteral(" differs from other ") + cls;
        } else {
            colourOf[cls] = colour;
        }
    }
    if (distinct) {
        for (auto a = colourOf.cbegin(); a != colourOf.cend(); ++a) {
            for (auto b = std::next(a); b != colourOf.cend(); ++b) {
                if (a.value() == b.value()) {
                    problems << a.key() + QStringLiteral(" and ") + b.key() + QStringLiteral(" look the same");
                }
            }
        }
    }
    return check(problems.isEmpty(), desc, problems.join(QStringLiteral("; ")));
}

// The help window: pages found, contents and index filled, every contents
// entry opens a page, keywords lead to their topics (also with F1 in the
// editor) and, with Qt Help, the full-text search finds something.  With
// LINCUPL_SELFTEST_HELP_PNG=<dir> a few pages are also saved as pictures in
// a light and a dark palette.
bool checkHelp() {
    bool ok = true;
    QTemporaryDir cacheDir;
    Help::setCacheDirectory(cacheDir.path());
    HelpWindow *help = HelpWindow::instance();
    ok &= check(help->isAvailable(), "Open the help pages", help->problem());
    if (!help->isAvailable()) {
        return false;
    }
    std::fprintf(stdout, "[INFO] help shown with %s\n", help->usesQtHelp() ? "Qt Help" : "the plain page viewer");

    const QList<QUrl> pages = help->contentsUrls();
    ok &= check(pages.size() >= 20, "Help contents tree is filled",
                QString::number(pages.size()));
    ok &= check(help->indexEntryCount() >= 100, "Help index is filled",
                QString::number(help->indexEntryCount()));
    QStringList empty;
    for (const QUrl &url : pages) {
        help->openUrl(url);
        if (help->browser()->toPlainText().trimmed().size() < 40 || help->browser()->documentTitle().isEmpty()) {
            empty << url.toString();
        }
    }
    ok &= check(empty.isEmpty(), "Every contents entry opens a page with a title", empty.join(QStringLiteral(", ")));

    struct WordAt { const char *line; int column; const char *keyword; };
    const WordAt words[] = {
        {"field addr = [a7..0];", 2, "FIELD"},
        {"$define WIDTH 8", 3, "$DEFINE"},
        {"q0.oe = en;", 4, ".oe"},
        {"[q3..0].d = [d3..0];", 9, ".d"},
        {"y = addr:&;", 9, ":&"},
        {"y = addr:&;", 10, ":&"},
        {"y = addr:&;", 6, "ADDR"},
        {"SEQUENCED count {", 17, ""},
        {"present 'b'01 next 'b'10;", 3, "PRESENT"},
    };
    QStringList wrong;
    for (const WordAt &w : words) {
        const QString got = Help::keywordAt(QString::fromLatin1(w.line), w.column);
        if (got != QLatin1String(w.keyword)) {
            wrong << QStringLiteral("'%1'@%2 -> '%3'").arg(QLatin1String(w.line)).arg(w.column).arg(got);
        }
    }
    ok &= check(wrong.isEmpty(), "Help keyword under the cursor", wrong.join(QStringLiteral("; ")));

    QStringList missing;
    for (const char *kw : {"PIN", "PINNODE", "NODE", "FIELD", "TABLE", "SEQUENCE", "SEQUENCED", "SEQUENCEJK",
                           "SEQUENCERS", "SEQUENCET", "PRESENT", "NEXT", "OUT", "DEFAULT", "CONDITION", "FUNCTION",
                           "APPEND", "MIN", "PROPERTY", "$DEFINE", "$UNDEF", "$INCLUDE", "$IFDEF", "$IFNDEF",
                           "$ELSE", "$ENDIF", "$MACRO", "$MEND", "$REPEAT", "$REPEND", ".d", ".oe", ".ar", ".sp",
                           ".t", ".ck", ".io", ":&", ":#", ":$", "ORDER", "VECTORS", "BASE", "$MSG", "$TRACE"}) {
        if (help->topicsForKeyword(QLatin1String(kw)).isEmpty()) {
            missing << QLatin1String(kw);
        }
    }
    ok &= check(missing.isEmpty(), "Index keywords lead to topics", missing.join(QStringLiteral(" ")));
    const QUrl fieldDesign = help->topicFor(QStringLiteral("FIELD"), Help::Context::Design);
    const QUrl fieldSim = help->topicFor(QStringLiteral("FIELD"), Help::Context::Simulator);
    ok &= check(fieldDesign.isValid() && fieldSim.isValid() && fieldDesign != fieldSim
                    && fieldSim.path().contains(QStringLiteral("sim")),
                "FIELD has a design and a simulator topic",
                fieldDesign.toString() + QStringLiteral(" / ") + fieldSim.toString());

    // F1 on a keyword in the editor opens its topic
    {
        CodeEditor editor;
        editor.setFilePath(QStringLiteral("example.pld"));
        editor.setPlainText(QStringLiteral("field addr = [a7..0];\n"));
        QTextCursor c = editor.textCursor();
        c.setPosition(2);
        editor.setTextCursor(c);
        help->openUrl(help->homeUrl());
        QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_F1, Qt::NoModifier);
        QApplication::sendEvent(&editor, &override);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_F1, Qt::NoModifier);
        QApplication::sendEvent(&editor, &press);
        ok &= check(override.isAccepted() && help->currentUrl().path() == fieldDesign.path(),
                    "F1 in the editor opens the topic of the keyword", help->currentUrl().toString());
    }

    if (help->usesQtHelp()) {
        const int hits = help->searchBlocking(QStringLiteral("minimization"));
        ok &= check(hits > 0, "Full-text search of the help finds pages", QString::number(hits));
    } else {
        std::fprintf(stdout, "[INFO] full-text search needs Qt Help; skipped\n");
    }

    help->resize(1000, 720);
    help->openUrl(help->homeUrl());
    ok &= check(!help->grab().isNull(), "Offscreen render of the help window");

    const QString pngDir = qEnvironmentVariable("LINCUPL_SELFTEST_HELP_PNG");
    if (!pngDir.isEmpty()) {
        QDir().mkpath(pngDir);
        const QPalette light = QApplication::palette();
        QPalette dark = light;
        dark.setColor(QPalette::Window, QColor(0x2b, 0x2b, 0x2b));
        dark.setColor(QPalette::WindowText, QColor(0xe6, 0xe6, 0xe6));
        dark.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
        dark.setColor(QPalette::Text, QColor(0xe6, 0xe6, 0xe6));
        dark.setColor(QPalette::Button, QColor(0x3a, 0x3a, 0x3a));
        dark.setColor(QPalette::ButtonText, QColor(0xe6, 0xe6, 0xe6));
        const QStringList shots = qEnvironmentVariable("LINCUPL_SELFTEST_HELP_PAGES",
            QStringLiteral("index.html lang-expressions.html sim-language.html")).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &page : shots) {
            for (int mode = 0; mode < 2; ++mode) {
                help->setPalette(mode == 0 ? light : dark);
                help->openUrl(help->pageUrl(page));
                const QString file = QDir(pngDir).filePath(
                    QFileInfo(page).completeBaseName() + (mode == 0 ? QStringLiteral("-light.png") : QStringLiteral("-dark.png")));
                help->grab().save(file);
            }
        }
        help->setPalette(light);
    }
    help->close();
    return ok;
}

// Flags added and removed between two flag lists
QStringList flagChange(const QStringList &before, const QStringList &after) {
    QStringList change;
    for (const QString &f : after) {
        if (!before.contains(f)) change << QStringLiteral("+") + f;
    }
    for (const QString &f : before) {
        if (!after.contains(f)) change << QStringLiteral("-") + f;
    }
    return change;
}

// Options > Compiler Options and Simulator Options: every box gives the
// flag that does what its label says, the defaults never program the
// security fuse, and the settings of earlier versions are converted
// without turning any of them into a different flag.
bool checkCompilerOptions() {
    bool ok = true;

    // what each box must do, written down independently of the dialog's table
    struct Expect {
        const char *key;
        const char *flag;
        const char *labelWord;
    };
    const QList<Expect> expect = {
        {"secureDevice", "-g", "Secure device"},
        {"deactivateUnusedOr", "-d", "Deactivate unused OR"},
        {"simulate", "-s", "Simulate"},
        {"oneHot", "-o", "One-hot"},
        {"jedecNameIsPldName", "-n", "named after the design file"},
        {"bestPolarity", "-kb", "polarity"},
        {"demorgan", "-kd", "DeMorgan"},
        {"keepXor", "-kx", "XOR"},
        {"shareTerms", "-ks", "sharing"},
        {"noSharing", "-r", "Never share"},
        {"jedec", "-j", "JEDEC"},
        {"hlFile", "-i", "HL"},
        {"asciiHex", "-h", "ASCII-hex"},
        {"edif", "-t", "EDIF"},
        {"palasm", "-c", "PALASM"},
        {"pla", "-b", "PLA"},
        {"pdif", "-p", "PDIF"},
        {"expandedMacro", "-e", "macros"},
        {"list", "-l", "Listing"},
        {"absolute", "-a", "Absolute"},
        {"equations", "-x", "Equations"},
        {"fusePlot", "-f", "Fuse plot"},
    };

    const QStringList defaultFlags = CompilerOptions{}.buildFlagArgs(false);
    ok &= check(defaultFlags == QStringList({QStringLiteral("-m1"), QStringLiteral("-j"), QStringLiteral("-l"),
                                             QStringLiteral("-a"), QStringLiteral("-x")}) &&
                    !defaultFlags.contains(QStringLiteral("-g")),
                "Default compiler flags are -m1 -j -l -a -x, without -g",
                defaultFlags.join(QLatin1Char(' ')));

    {
        CompilerOptionsDialog dlg(CompilerOptions{}, nullptr);
        QStringList names;
        for (QCheckBox *cb : dlg.findChildren<QCheckBox *>()) names << cb->objectName();
        QStringList expectedNames;
        for (const Expect &e : expect) expectedNames << QLatin1String(e.key);
        names.sort();
        expectedNames.sort();
        ok &= check(names == expectedNames, "Every compiler option box is one of the checked ones",
                    names.join(QLatin1Char(' ')));

        QCheckBox *secure = dlg.findChild<QCheckBox *>(QStringLiteral("secureDevice"));
        ok &= check(secure && !secure->isChecked() && secure->text().contains(QStringLiteral("security fuse")),
                    "Secure Device is off by default and names the security fuse",
                    secure ? secure->text() : QString());
    }

    QStringList wrong;
    for (const Expect &e : expect) {
        CompilerOptionsDialog dlg(CompilerOptions{}, nullptr);
        QCheckBox *cb = dlg.findChild<QCheckBox *>(QLatin1String(e.key));
        if (!cb) {
            wrong << QLatin1String(e.key) + QStringLiteral(" missing");
            continue;
        }
        const QStringList before = dlg.options().buildFlagArgs(false);
        const bool wasOn = cb->isChecked();
        cb->setChecked(!wasOn);
        const QStringList change = flagChange(before, dlg.options().buildFlagArgs(false));
        const QString want = (wasOn ? QStringLiteral("-") : QStringLiteral("+")) + QLatin1String(e.flag);
        if (change != QStringList{want} || !cb->text().contains(QLatin1String(e.labelWord)) ||
            !cb->toolTip().contains(QStringLiteral("cupl ") + QLatin1String(e.flag) + QStringLiteral("<"))) {
            wrong << QStringLiteral("%1 \"%2\": %3").arg(QLatin1String(e.key), cb->text(),
                                                         change.join(QLatin1Char(' ')));
        }
    }
    ok &= check(wrong.isEmpty(), "Each compiler option box adds exactly its own flag",
                wrong.join(QStringLiteral("; ")));

    {
        CompilerOptionsDialog dlg(CompilerOptions{}, nullptr);
        QStringList levels;
        for (int level = 0; level <= 4; ++level) {
            if (auto *r = dlg.findChild<QRadioButton *>(QStringLiteral("minimization%1").arg(level))) {
                r->setChecked(true);
                levels << dlg.options().buildFlagArgs(false).first();
            }
        }
        ok &= check(levels == QStringList({QStringLiteral("-m0"), QStringLiteral("-m1"), QStringLiteral("-m2"),
                                           QStringLiteral("-m3"), QStringLiteral("-m4")}),
                    "Minimization buttons give -m0 .. -m4", levels.join(QLatin1Char(' ')));

        auto *kb = dlg.findChild<QCheckBox *>(QStringLiteral("bestPolarity"));
        auto *kd = dlg.findChild<QCheckBox *>(QStringLiteral("demorgan"));
        kb->setChecked(true);
        kd->setChecked(true);
        const QStringList flags = dlg.options().buildFlagArgs(false);
        ok &= check(!kb->isChecked() && flags.contains(QStringLiteral("-kd")) && !flags.contains(QStringLiteral("-kb")),
                    "Best polarity and DeMorgan exclude each other", flags.join(QLatin1Char(' ')));
    }

    {
        CompilerOptions o;
        o.secureDevice = true;
        o.fusePlot = true;
        o.list = false;
        o.pla = true;
        const QStringList flags = o.buildFlagArgs(true);
        ok &= check(!flags.contains(QStringLiteral("-g")) && !flags.contains(QStringLiteral("-j")) &&
                        !flags.contains(QStringLiteral("-f")) && flags.contains(QStringLiteral("-b")) &&
                        flags.contains(QStringLiteral("-a")) && flags.contains(QStringLiteral("-l")) &&
                        flags.contains(QStringLiteral("-x")),
                    "Virtual device: no download flags, always -a -l -x", flags.join(QLatin1Char(' ')));
    }

    // settings of earlier versions
    {
        QSettings s;
        s.remove(QStringLiteral("compilerFlags"));
        s.remove(QStringLiteral("compiler"));
        const QStringList dropped = {
            QStringLiteral("gatesFile"), QStringLiteral("docFusePlot"),
            QStringLiteral("allDownloadFiles"), QStringLiteral("deactivateUnusedOr"),
            QStringLiteral("warningsOff"), QStringLiteral("subtleWarningsOff")};
        for (const QString &k : dropped) s.setValue(QStringLiteral("compiler/") + k, true);
        s.setValue(QStringLiteral("compiler/minimizationLevel"), 3);
        s.setValue(QStringLiteral("compiler/demorganMode"), 1);
        s.setValue(QStringLiteral("compiler/plaFile"), true);
        s.setValue(QStringLiteral("compiler/docTruthTable"), true);
        s.setValue(QStringLiteral("compiler/centralFuseNums"), true);
        s.setValue(QStringLiteral("compiler/functionFile"), true);
        s.setValue(QStringLiteral("compiler/listingFile"), false);
        s.setValue(QStringLiteral("compiler/waveformAfterCompile"), true);
        s.sync();

        CompilerOptions o;
        o.loadFromSettings();
        const QStringList flags = o.buildFlagArgs(false);
        const QStringList expectFlags = {QStringLiteral("-m3"), QStringLiteral("-s"), QStringLiteral("-j"),
                                         QStringLiteral("-t"), QStringLiteral("-c"), QStringLiteral("-b"),
                                         QStringLiteral("-a"), QStringLiteral("-x"), QStringLiteral("-f")};
        ok &= check(!o.secureDevice && flags == expectFlags,
                    "Old compiler settings: Gate-Level Equations never becomes Secure Device, "
                    "boxes whose label did not match their flag are dropped",
                    flags.join(QLatin1Char(' ')));

        s.sync();
        s.beginGroup(QStringLiteral("compiler"));
        const bool legacyLeft = !s.childKeys().isEmpty();
        s.endGroup();
        CompilerOptions again;
        again.loadFromSettings();
        ok &= check(!legacyLeft && s.value(QStringLiteral("compilerFlags/version")).toInt() == 1 &&
                        again.buildFlagArgs(false) == expectFlags,
                    "Old compiler settings are removed after the conversion");

        s.remove(QStringLiteral("compilerFlags"));
        s.setValue(QStringLiteral("compiler/gatesFile"), true);
        CompilerOptions onlyG;
        onlyG.loadFromSettings();
        ok &= check(onlyG.buildFlagArgs(false) == defaultFlags,
                    "Old gatesFile=true alone gives the default flags");

        CompilerOptions secure;
        secure.secureDevice = true;
        secure.saveToSettings();
        CompilerOptions reloaded;
        reloaded.loadFromSettings();
        ok &= check(reloaded.secureDevice && reloaded.buildFlagArgs(false).contains(QStringLiteral("-g")),
                    "Secure Device is saved and read back");
        s.remove(QStringLiteral("compilerFlags"));
    }

    // simulator
    {
        ok &= check(SimulatorOptions{}.buildFlagArgs(false) == QStringList{QStringLiteral("-l")},
                    "Default simulator flags are -l",
                    SimulatorOptions{}.buildFlagArgs(false).join(QLatin1Char(' ')));
        SimulatorOptionsDialog dlg(SimulatorOptions{}, nullptr);
        QStringList wrongSim;
        const QList<QPair<QString, QString>> simExpect = {
            {QStringLiteral("writeListing"), QStringLiteral("-l")},
            {QStringLiteral("appendVectorsToJedec"), QStringLiteral("-j")},
            {QStringLiteral("usePldFilename"), QStringLiteral("-n")},
            {QStringLiteral("autoOpenLinSim"), QString()},
        };
        for (const auto &e : simExpect) {
            auto *cb = dlg.findChild<QCheckBox *>(e.first);
            if (!cb) {
                wrongSim << e.first + QStringLiteral(" missing");
                continue;
            }
            const QStringList before = dlg.options().buildFlagArgs(false);
            const bool wasOn = cb->isChecked();
            cb->setChecked(!wasOn);
            const QStringList change = flagChange(before, dlg.options().buildFlagArgs(false));
            const QStringList want = e.second.isEmpty()
                ? QStringList{}
                : QStringList{(wasOn ? QStringLiteral("-") : QStringLiteral("+")) + e.second};
            if (change != want) wrongSim << e.first + QStringLiteral(": ") + change.join(QLatin1Char(' '));
            cb->setChecked(wasOn);
        }
        ok &= check(wrongSim.isEmpty() && dlg.findChildren<QCheckBox *>().size() == simExpect.size(),
                    "Each simulator option box adds exactly its own flag", wrongSim.join(QStringLiteral("; ")));

        QSettings s;
        s.remove(QStringLiteral("simulatorFlags"));
        s.setValue(QStringLiteral("simulator/listingFile"), false);
        s.setValue(QStringLiteral("simulator/waveformFile"), true);
        s.setValue(QStringLiteral("simulator/verboseOutput"), true);
        s.setValue(QStringLiteral("simulator/usePldFilename"), true);
        SimulatorOptions o;
        o.loadFromSettings();
        s.beginGroup(QStringLiteral("simulator"));
        const bool legacyLeft = !s.childKeys().isEmpty();
        s.endGroup();
        ok &= check(o.buildFlagArgs(false) == QStringList({QStringLiteral("-l"), QStringLiteral("-n")}) && !legacyLeft,
                    "Old simulator settings: listing or waveform become -l, Verbose is dropped",
                    o.buildFlagArgs(false).join(QLatin1Char(' ')));
        s.remove(QStringLiteral("simulatorFlags"));
    }

    const QString pngDir = qEnvironmentVariable("LINCUPL_SELFTEST_OPTIONS_PNG");
    if (!pngDir.isEmpty()) {
        QDir().mkpath(pngDir);
        CompilerOptionsDialog dlg(CompilerOptions{}, nullptr);
        dlg.grab().save(QDir(pngDir).filePath(QStringLiteral("compiler-options.png")));
        SimulatorOptionsDialog sim(SimulatorOptions{}, nullptr);
        sim.grab().save(QDir(pngDir).filePath(QStringLiteral("simulator-options.png")));
    }
    return ok;
}

} // namespace

int run() {
    bool allOk = true;

    // Isolate QSettings from the user's real configuration: a saved custom
    // library / bin dir must not influence the test, and the test must not
    // pollute the user's recent-files list with deleted temp paths.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "[FAIL] Create isolated QSettings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    {
        QSettings probe;
        allOk &= check(probe.fileName().startsWith(settingsDir.path()),
                       "QSettings isolated to temporary directory", probe.fileName());
    }

    // Live tool runs catch broken sibling-binary discovery.
    // Set LINCUPL_SELFTEST_ALLOW_MISSING_TOOLS=1 to permit a GUI-only standalone build.
    const bool allowMissingTools =
        qEnvironmentVariable("LINCUPL_SELFTEST_ALLOW_MISSING_TOOLS") == QStringLiteral("1");
    for (const QString &tool : {QStringLiteral("cupl"), QStringLiteral("csim"), QStringLiteral("backpin")}) {
        const QString found = ToolPaths::findExecutable(tool);
        if (found.isEmpty() && allowMissingTools) {
            std::fprintf(stdout, "[INFO] '%s' not found; allowed by LINCUPL_SELFTEST_ALLOW_MISSING_TOOLS=1\n",
                         tool.toUtf8().constData());
            continue;
        }
        allOk &= check(!found.isEmpty(),
                       QStringLiteral("Discover sibling '%1' executable").arg(tool).toUtf8().constData(),
                       found);
    }

    // 1. Verify .dl binary parsing on Atmel.dl AND cupl.dl + Atmel.cat grouping
    const QStringList dlFiles = ToolPaths::findDeviceLibraries();
    allOk &= check(!dlFiles.isEmpty(), "Discover .dl device libraries",
                   dlFiles.join(QStringLiteral(", ")));

    QString atmelDl;
    QString cuplDl;
    QString mergedDl;
    for (const QString &p : dlFiles) {
        const QString fn = QFileInfo(p).fileName();
        if (fn.compare(QStringLiteral("Atmel.dl"), Qt::CaseInsensitive) == 0 && atmelDl.isEmpty()) {
            atmelDl = p;
        } else if (fn.compare(QStringLiteral("cupl.dl"), Qt::CaseInsensitive) == 0 && cuplDl.isEmpty()) {
            cuplDl = p;
        } else if (fn.compare(QStringLiteral("lincupl.dl"), Qt::CaseInsensitive) == 0 && mergedDl.isEmpty()) {
            mergedDl = p;
        }
    }

    const QString catPath = ToolPaths::findAtmelCat();
    allOk &= check(!catPath.isEmpty(), "Locate Atmel.cat category file", catPath);

    if (!atmelDl.isEmpty()) {
        DeviceLibrary lib;
        QString err;
        const bool loaded = lib.loadLibrary(atmelDl, &err);
        allOk &= check(loaded, "Parse Atmel.dl binary library", err);
        if (!catPath.isEmpty()) {
            allOk &= check(lib.loadCategories(catPath, &err), "Parse Atmel.cat categories", err);
        }
        allOk &= check(lib.devices().size() >= 80, "Atmel.dl device count >= 80",
                       QString::number(lib.devices().size()));

        const DeviceInfo g16 = lib.findDevice(QStringLiteral("g16v8a"));
        allOk &= check(g16.isValid() && g16.pins == 20 && g16.fuses == 2194 && g16.productTerms == 64,
                       "Atmel.dl g16v8a metadata (20 pins, 2194 fuses, 64 pterms, GAL DIP)",
                       QStringLiteral("pins=%1 fuses=%2 pterms=%3 cat=%4 pkg=%5")
                           .arg(g16.pins).arg(g16.fuses).arg(g16.productTerms)
                           .arg(g16.category, g16.packageGroup));
        allOk &= check(g16.category == QStringLiteral("GAL") &&
                           g16.packageGroup == QStringLiteral("DIP / Standard"),
                       "Atmel.cat maps g16v8a to GAL / DIP");

        const DeviceInfo g22lcc = lib.findDevice(QStringLiteral("g22v10lcc"));
        allOk &= check(g22lcc.isValid() && g22lcc.packageGroup == QStringLiteral("LCC / PLCC"),
                       "Atmel.cat maps g22v10lcc to LCC / PLCC");
    }

    if (!cuplDl.isEmpty()) {
        DeviceLibrary fullLib;
        QString err;
        allOk &= check(fullLib.loadLibrary(cuplDl, &err) && fullLib.devices().size() >= 600,
                       "Parse cupl.dl binary library (>= 600 devices)",
                       err.isEmpty() ? QString::number(fullLib.devices().size()) : err);
    }

    // lincupl.dl, made at build time: Atmel.dl's devices, then those only
    // cupl.dl has; it is the library chosen by default
    if (!atmelDl.isEmpty() && !cuplDl.isEmpty()) {
        allOk &= check(!mergedDl.isEmpty(), "Discover lincupl.dl next to Atmel.dl and cupl.dl",
                       dlFiles.join(QStringLiteral(", ")));
    }
    if (!mergedDl.isEmpty()) {
        DeviceLibrary mergedLib;
        QString err;
        const bool loaded = mergedLib.loadLibrary(mergedDl, &err);
        allOk &= check(loaded && mergedLib.devices().size() >= 650,
                       "Parse lincupl.dl binary library (>= 650 devices)",
                       err.isEmpty() ? QString::number(mergedLib.devices().size()) : err);
        const DeviceInfo g16 = mergedLib.findDevice(QStringLiteral("g16v8a"));
        const DeviceInfo p16 = mergedLib.findDevice(QStringLiteral("p16l8"));
        allOk &= check(g16.isValid() && g16.fuses == 2194 && p16.isValid() && p16.pins == 20,
                       "lincupl.dl has g16v8a (Atmel.dl) and p16l8 (cupl.dl)");
        if (qEnvironmentVariable("LIBCUPL").isEmpty()) {
            allOk &= check(QFileInfo(ToolPaths::activeDeviceLibrary()).fileName() == QStringLiteral("lincupl.dl"),
                           "lincupl.dl is the default library", ToolPaths::activeDeviceLibrary());
        }
    }

    // 2. Verify CuplErr.txt catalog loading & diagnostic parsing
    const QString errTxtPath = ToolPaths::findCuplErrTxt();
    allOk &= check(!errTxtPath.isEmpty(), "Locate CuplErr.txt error catalog", errTxtPath);
    ErrorCatalog errCat;
    {
        QString err;
        allOk &= check(errCat.loadCatalog(errTxtPath, &err) && errCat.entries().size() >= 100,
                       "Load CuplErr.txt entries (>= 100)",
                       err.isEmpty() ? QString::number(errCat.entries().size()) : err);
        const CuplErrorEntry e07 = errCat.lookup(QStringLiteral("0007ca"));
        const CuplErrorEntry e19 = errCat.lookup(QStringLiteral("0019sa"));
        const CuplErrorEntry ek7 = errCat.lookup(QStringLiteral("[0007w]"));
        allOk &= check(e07.isValid() && e19.isValid() && ek7.isValid() && ek7.code == QStringLiteral("0007ck"),
                       "Lookup compiler, simulator, and kernel shorthand error codes");
    }

    // 3. Verify Compiler & Simulator option flag generation
    allOk &= checkCompilerOptions();

    // 4. Verify New Design & New .SI Wizards
    QTemporaryDir tempDir;
    allOk &= check(tempDir.isValid(), "Create temporary workspace directory");
    if (!tempDir.isValid()) {
        return 1;
    }

    NewDesignSpec wizSpec;
    wizSpec.name = QStringLiteral("WizGates");
    wizSpec.device = QStringLiteral("g16v8a");
    wizSpec.inputs = {{QStringLiteral("1"), QStringLiteral("a"), false, QStringLiteral("Input A")},
                      {QStringLiteral("2"), QStringLiteral("b"), false, QStringLiteral("Input B")}};
    wizSpec.outputs = {{QStringLiteral("12"), QStringLiteral("y"), false, QStringLiteral("AND Output")}};
    const QString generatedPld = NewDesignDialog::generatePldTemplate(wizSpec);
    allOk &= check(generatedPld.contains(QStringLiteral("Device   g16v8a")) &&
                       generatedPld.contains(QStringLiteral("y = a & b")),
                   "NewDesignDialog generates valid CUPL .PLD template");

    const PldDesignSummary parsedSummary = NewSimDialog::parsePldForSimulation(generatedPld);
    const QString generatedSi = NewSimDialog::generateSiTemplate(parsedSummary, 4);
    allOk &= check(parsedSummary.signalsList.size() == 3 &&
                       generatedSi.contains(QStringLiteral("ORDER:")) &&
                       generatedSi.contains(QStringLiteral("VECTORS:")),
                   "NewSimDialog parses PINs/equations and scaffolds .SI vectors");


    // Syntax highlighting, token by token
    allOk &= checkHighlighting("CUPL highlighting of a design file",
        QStringLiteral(
            "Name     Test; /* PIN in a comment */\n"                  // 0
            "PIN 2 = clk;\n"                                           // 1
            "Y.d = !A & B # C $ D;\n"                                  // 2
            "FIELD f = [Q3..0];\n"                                     // 3
            "$DEFINE ON 'b'1\n"                                        // 4
            "x = f:'h'F0 & name.oe;\n"                                 // 5
            "Device g22v10; /* starts\n"                               // 6
            "   PIN still comment */ Z = q.ar & r.foo;\n"              // 7
            "$REPEAT i = [0..3]\n"                                     // 8
            "Q{i}.d = Q{i}.oe;\n"),                                    // 9
        CuplHighlighter::Language::Pld,
        {
            {0, "Name", 0, "header"}, {0, "Test", 0, "plain"}, {0, "PIN", 0, "comment"},
            {1, "PIN", 0, "keyword"}, {1, "2", 0, "number"}, {1, "=", 0, "assign"},
            {1, "clk", 0, "plain"},
            {2, "Y", 0, "plain"}, {2, ".d", 0, "extension"}, {2, "!", 0, "logic"},
            {2, "&", 0, "logic"}, {2, "#", 0, "logic"}, {2, "$", 0, "logic"},
            {3, "FIELD", 0, "keyword"}, {3, "..", 0, "assign"}, {3, "Q3", 0, "plain"},
            {4, "$DEFINE", 0, "directive"}, {4, "'b'1", 0, "number"},
            {5, ":", 0, "assign"}, {5, "'h'F0", 0, "number"}, {5, ".oe", 0, "extension"},
            {6, "Device", 0, "header"}, {6, "/* starts", 0, "comment"},
            {7, "PIN still comment */", 0, "comment"}, {7, "Z", 0, "plain"},
            {7, ".ar", 0, "extension"}, {7, ".foo", 0, "plain"},
            {8, "$REPEAT", 0, "directive"},
            {9, "{i}", 0, "directive"}, {9, ".oe", 0, "extension"},
        },
        true);
    allOk &= checkHighlighting("CUPL highlighting of a simulator input file",
        QStringLiteral(
            "Device g16v8;\n"                                          // 0
            "ORDER: a, %2, b, c, y;\n"                                 // 1
            "VECTORS:\n"                                               // 2
            "0 C 'F' H \"3\" X /* comment */\n"                        // 3
            "$MSG \"note\";\n"                                         // 4
            "1 K '0' L \"0\" Z\n"),                                    // 5
        CuplHighlighter::Language::Si,
        {
            {0, "Device", 0, "header"}, {1, "ORDER", 0, "keyword"}, {1, "%2", 0, "number"},
            {1, "a", 0, "plain"}, {2, "VECTORS", 0, "keyword"},
            {3, "0", 0, "input"}, {3, "C", 0, "clock"}, {3, "'F'", 0, "input"},
            {3, "H", 0, "output"}, {3, "\"3\"", 0, "output"}, {3, "X", 0, "other"},
            {3, "comment", 0, "comment"},
            {4, "$MSG", 0, "directive"}, {4, "\"note\"", 0, "string"},
            {5, "1", 0, "input"}, {5, "K", 0, "clock"}, {5, "L", 0, "output"},
            {5, "Z", 0, "other"},
        },
        false);
    allOk &= checkHighlighting("CUPL highlighting of a simulator listing",
        QStringLiteral(
            "LinCUPL " LINCUPL_VERSION " csim: CUPL Simulation Program\n" // 0
            "   1: Device g16v8; /* x */\n"                            // 1
            "===================\n"                                    // 2
            "0001: 0000  LLHH\n"                                       // 3
            "            ^^ ^\n"                                       // 4
            "[0019sa] user expected (H) for x\n"),                     // 5
        CuplHighlighter::Language::SiListing,
        {
            {0, "LinCUPL", 0, "banner"}, {1, "1:", 0, "banner"}, {1, "Device", 0, "header"},
            {1, "/* x */", 0, "comment"}, {2, "===", 0, "banner"}, {3, "0001:", 0, "banner"},
            {3, "0000", 0, "input"}, {3, "LLHH", 0, "output"}, {4, "^^", 0, "error"},
            {5, "[0019sa] user", 0, "error"},
        },
        true);

    // 5. Instantiate MainWindow, dialogs, syntax highlighter, and offscreen rendering
    MainWindow win;
    win.compilerOptions() = CompilerOptions{};
    win.compilerOptions().pla = true; // Exercise .pla output tab too
    win.simulatorOptions() = SimulatorOptions{};
    win.simulatorOptions().autoOpenLinSim = false;
    win.show();
    QApplication::processEvents();

    // Copy real COUNT10 example (or fallback to wizard design) into tempDir
    const QString examplesDir = ToolPaths::findExamplesDir();
    const QString tempPldPath = QDir(tempDir.path()).filePath(QStringLiteral("COUNT10.PLD"));
    const QString tempSiPath = QDir(tempDir.path()).filePath(QStringLiteral("COUNT10.SI"));

    if (!examplesDir.isEmpty() &&
        QFileInfo::exists(QDir(examplesDir).filePath(QStringLiteral("COUNT10.PLD")))) {
        QFile::copy(QDir(examplesDir).filePath(QStringLiteral("COUNT10.PLD")), tempPldPath);
        QFile::copy(QDir(examplesDir).filePath(QStringLiteral("COUNT10.SI")), tempSiPath);
    } else {
        QFile fp(tempPldPath);
        if (fp.open(QIODevice::WriteOnly)) fp.write(generatedPld.toLatin1());
        QFile fs(tempSiPath);
        if (fs.open(QIODevice::WriteOnly)) fs.write(generatedSi.toLatin1());
    }

    allOk &= check(win.openFile(tempPldPath, true), "Open .PLD in MainWindow editor", tempPldPath);
    QApplication::processEvents();

    // Verify CuplHighlighter produced syntax format ranges on non-empty blocks
    {
        CodeEditor *pldEd = win.editorForPath(tempPldPath);
        int highlightedBlocks = 0;
        if (pldEd) {
            for (QTextBlock blk = pldEd->document()->begin(); blk.isValid(); blk = blk.next()) {
                if (blk.layout() && !blk.layout()->formats().isEmpty()) {
                    ++highlightedBlocks;
                }
            }
        }
        allOk &= check(highlightedBlocks >= 5, "CuplHighlighter applies syntax format spans",
                       QStringLiteral("highlightedBlocks=%1").arg(highlightedBlocks));
    }

    {
        const QPixmap shot = win.grab();
        allOk &= check(!shot.isNull() && shot.width() > 200, "Offscreen render of MainWindow & CodeEditor");

        DeviceBrowserDialog devDlg(&win.deviceLibrary(), QStringLiteral("g16v8a"), true, &win);
        devDlg.show();
        QApplication::processEvents();
        allOk &= check(!devDlg.grab().isNull() && devDlg.selectedMnemonic() == QStringLiteral("g16v8a"),
                       "Offscreen render & selection in DeviceBrowserDialog");

        ErrorViewerDialog errDlg(&win.errorCatalog(), QStringLiteral("0009ca"), &win);
        errDlg.show();
        QApplication::processEvents();
        allOk &= check(!errDlg.grab().isNull(), "Offscreen render of ErrorViewerDialog");
    }

    // Options > Font...: editor, message and sidebar fonts, kept in QSettings;
    // zooming stays relative to the chosen editor font.
    {
        FontSettings fonts;
        fonts.editor = QFont(QStringLiteral("Courier New"), 17);
        fonts.editor.setBold(true);
        fonts.messages = QFont(QStringLiteral("Courier New"), 9);
        fonts.messages.setItalic(true);
        fonts.sidebar = QFont(QStringLiteral("Helvetica"), 15);
        fonts.saveToSettings();
        FontSettings reloaded;
        reloaded.loadFromSettings();
        allOk &= check(reloaded == fonts, "Font settings are saved to and read from QSettings",
                       reloaded.editor.toString());
        win.applyFontSettings(reloaded);

        CodeEditor *pldEd = win.editorForPath(tempPldPath);
        bool editorsOk = win.tabCount() > 0;
        for (int i = 0; i < win.tabCount(); ++i) {
            const QFont f = win.editorAt(i)->font();
            editorsOk &= f.pointSize() == 17 && f.bold() && f.family() == QStringLiteral("Courier New");
        }
        QPlainTextEdit *buildLog = nullptr;
        QTreeWidget *diagTree = nullptr;
        QTreeWidget *sidebar = nullptr;
        for (QPlainTextEdit *pe : win.findChildren<QPlainTextEdit *>()) {
            if (!qobject_cast<CodeEditor *>(pe)) buildLog = pe;
        }
        for (QTreeWidget *tw : win.findChildren<QTreeWidget *>()) {
            (tw->columnCount() == 5 ? diagTree : sidebar) = tw;
        }
        allOk &= check(editorsOk && buildLog && buildLog->font().italic() && buildLog->font().pointSize() == 9 &&
                           diagTree && diagTree->font().italic() &&
                           sidebar && sidebar->font().pointSize() == 15,
                       "Font settings reach editors, build log, diagnostics and sidebar");

        pldEd->zoomBy(2);
        const int zoomed = pldEd->font().pointSize();
        fonts.editor.setPointSize(12);
        win.applyFontSettings(fonts);
        win.openFile(tempSiPath, false);
        CodeEditor *siEd = win.editorForPath(tempSiPath);
        allOk &= check(zoomed == 19 && pldEd->font().pointSize() == 14 &&
                           siEd && siEd->font().pointSize() == 12 && siEd->font().bold(),
                       "Editor zoom stays relative to a newly chosen font; new tabs use it",
                       QStringLiteral("zoomed=%1 after=%2 new=%3").arg(zoomed).arg(pldEd->font().pointSize())
                           .arg(siEd ? siEd->font().pointSize() : -1));

        const FontSettings defaults;
        defaults.saveToSettings();
        QSettings s;
        allOk &= check(!s.contains(QStringLiteral("fonts/editor")) && !s.contains(QStringLiteral("fonts/sidebar")),
                       "Reset to defaults removes the saved fonts");
        win.applyFontSettings(defaults);
        pldEd->zoomBy(-2);
        allOk &= check(pldEd->font() == FontSettings::defaultEditorFont(),
                       "Default editor font restored", pldEd->font().toString());
    }

    // 6. Run cupl & csim via MainWindow QProcess and verify output tabs (.doc, .jed, .lst, .abs, .pla, .so)
    const QString cuplExe = ToolPaths::findExecutable(QStringLiteral("cupl"));
    if (!cuplExe.isEmpty()) {
        win.startCompile(false);
        const bool finished = win.waitForActiveProcess(15000);
        allOk &= check(finished && win.lastRunSucceeded() && win.lastProcessExitCode() == 0,
                       "Run cupl compile via QProcess",
                       win.buildLogText().right(300));

        // Check that companion tabs loaded (.doc, .jed, .lst, .abs, .pla)
        bool hasDoc = false, hasJed = false, hasLst = false, hasAbs = false, hasPla = false;
        for (int i = 0; i < win.tabCount(); ++i) {
            CodeEditor *ed = win.editorAt(i);
            if (!ed || ed->toPlainText().isEmpty()) continue;
            const QString ext = QFileInfo(ed->filePath()).suffix().toLower();
            if (ext == QStringLiteral("doc")) hasDoc = true;
            if (ext == QStringLiteral("jed")) hasJed = ed->toPlainText().contains(QStringLiteral("*QF"));
            if (ext == QStringLiteral("lst")) hasLst = true;
            if (ext == QStringLiteral("abs")) hasAbs = true;
            if (ext == QStringLiteral("pla")) hasPla = true;
        }
        allOk &= check(hasDoc && hasJed && hasLst && hasAbs && hasPla,
                       "Auto-load generated .doc, .jed, .lst, .abs, and .pla output tabs",
                       QStringLiteral("doc=%1 jed=%2 lst=%3 abs=%4 pla=%5")
                           .arg(hasDoc).arg(hasJed).arg(hasLst).arg(hasAbs).arg(hasPla));

        // The .abs tab shows the decoded design; Raw switches to the hex dump and back
        {
            CodeEditor *absEd = nullptr;
            for (int i = 0; i < win.tabCount(); ++i) {
                CodeEditor *ed = win.editorAt(i);
                if (ed && QFileInfo(ed->filePath()).suffix().compare(QStringLiteral("abs"), Qt::CaseInsensitive) == 0) {
                    absEd = ed;
                }
            }
            const QString decoded = absEd ? absEd->toPlainText() : QString();
            const bool decodedOk = absEd && absEd->hasAlternateViews() && !absEd->showsRaw() &&
                                   decoded.startsWith(QStringLiteral("Absolute file COUNT10.abs")) &&
                                   decoded.contains(QStringLiteral("  design         Count10")) &&
                                   decoded.contains(QStringLiteral("Symbols (")) &&
                                   decoded.contains(QStringLiteral("drives carry (pin 18): csim takes the OR array")) &&
                                   decoded.contains(QStringLiteral("    term  Q0 Q1 Q2 Q3 clr dir")) &&
                                   decoded.contains(QStringLiteral("  FIELD count = ")) &&
                                   !decoded.contains(QStringLiteral("*** error"));
            allOk &= check(decodedOk, "The .abs tab shows the decoded design", decoded.left(300));
            bool rawOk = false;
            bool backOk = false;
            if (absEd && absEd->rawButton()) {
                absEd->rawButton()->click();
                const QString raw = absEd->toPlainText();
                rawOk = absEd->showsRaw() && raw.contains(QStringLiteral("shown as hex")) &&
                        raw.contains(QStringLiteral("00000000  25 5b 05 00")) && !absEd->document()->isModified();
                absEd->rawButton()->click();
                backOk = !absEd->showsRaw() && absEd->toPlainText() == decoded;
            }
            allOk &= check(rawOk && backOk, "Raw toggles the .abs tab between the hex dump and the decoded view",
                           QStringLiteral("raw=%1 back=%2").arg(rawOk).arg(backOk));
        }

        const QString csimExe = ToolPaths::findExecutable(QStringLiteral("csim"));
        if (!csimExe.isEmpty() && QFileInfo::exists(tempSiPath)) {
            win.openFile(tempSiPath, true);
            win.startSimulate(false);
            const bool simDone = win.waitForActiveProcess(15000);
            allOk &= check(simDone && win.lastRunSucceeded() && win.lastProcessExitCode() == 0,
                           "Run csim simulation via QProcess",
                           win.buildLogText().right(300));

            bool hasSo = false;
            for (int i = 0; i < win.tabCount(); ++i) {
                CodeEditor *ed = win.editorAt(i);
                if (ed && QFileInfo(ed->filePath()).suffix().compare(QStringLiteral("so"), Qt::CaseInsensitive) == 0 &&
                    !ed->toPlainText().isEmpty()) {
                    hasSo = true;
                }
            }
            allOk &= check(hasSo, "Auto-load generated .so simulation listing tab");
        }

        // 7. Test diagnostic parsing + click-to-line navigation on intentional syntax error
        const QString badPldPath = QDir(tempDir.path()).filePath(QStringLiteral("BROKEN.PLD"));
        {
            QFile bf(badPldPath);
            if (bf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                bf.write("Name Broken;\n"
                         "Partno 01;\n"
                         "Date 01/01/26;\n"
                         "Revision 01;\n"
                         "Designer Test;\n"
                         "Company Test;\n"
                         "Assembly None;\n"
                         "Location None;\n"
                         "Device g16v8a;\n"
                         "Pin 1 = a\n"
                         "Pin 12 = y;\n"
                         "y = a;\n");
                bf.close();
            }
        }
        win.openFile(badPldPath, true);
        win.startCompile(false);
        win.waitForActiveProcess(15000);
        allOk &= check(!win.currentDiagnostics().isEmpty(),
                       "Diagnostics captured on broken .PLD compile",
                       QStringLiteral("count=%1").arg(win.currentDiagnostics().size()));
        {
            // cupl closes a failed compile with "Fatal CUPL errors encountered ...";
            // the compile counts as failed and the line is not a diagnostic of its own
            bool closingLineAsDiag = false;
            for (const DiagnosticItem &d : win.currentDiagnostics()) {
                closingLineAsDiag |= d.message.contains(QStringLiteral("errors encountered"));
            }
            allOk &= check(!win.lastRunSucceeded() && win.lastProcessExitCode() == 1 && !closingLineAsDiag &&
                               win.buildLogText().contains(QStringLiteral("Fatal CUPL errors encountered ... program aborted!")),
                           "A failed compile ends with cupl's closing message and is reported as failed",
                           win.buildLogText().right(300));
        }
        if (!win.currentDiagnostics().isEmpty()) {
            win.activateDiagnostic(0);
            CodeEditor *ed = win.currentEditor();
            const int jumpedLine = ed ? (ed->textCursor().blockNumber() + 1) : 0;
            allOk &= check(jumpedLine >= 10 && jumpedLine <= 11,
                           "Diagnostic activation jumps editor cursor to error line",
                           QStringLiteral("line=%1").arg(jumpedLine));
        }

        // 7b. A design the ATF1502 fitter cannot fit: the compile counts as failed
        if (!ToolPaths::findExecutable(QStringLiteral("fit1502")).isEmpty()) {
            const QString noFitPath = QDir(tempDir.path()).filePath(QStringLiteral("NOFIT.PLD"));
            {
                QFile nf(noFitPath);
                if (nf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    nf.write("Name NoFit; Partno x; Date x; Revision 01; Designer x; Company x; Assembly x; "
                             "Location x;\nDevice f1502ispplcc44;\n");
                    for (int i = 0; i < 21; ++i) {
                        nf.write(QStringLiteral("Pin = a%1;\nPin = y%1;\n").arg(i).toLatin1());
                    }
                    for (int i = 0; i < 21; ++i) {
                        nf.write(QStringLiteral("y%1 = a%1 & a%2;\n").arg(i).arg((i + 1) % 21).toLatin1());
                    }
                    nf.close();
                }
            }
            win.openFile(noFitPath, true);
            win.startCompile(false);
            const bool noFitDone = win.waitForActiveProcess(30000);
            bool fitterDiag = false;
            for (const DiagnosticItem &d : win.currentDiagnostics()) {
                fitterDiag |= d.code == QStringLiteral("0010ck") && d.severity == QStringLiteral("Fatal");
            }
            allOk &= check(noFitDone && !win.lastRunSucceeded() && win.lastProcessExitCode() == 1 && fitterDiag,
                           "A compile whose fitter cannot fit the design is reported as failed",
                           win.buildLogText().right(400));
        }

        // 8. Test Wizard-generated .PLD + .SI with Device-Independent Compile & Simulate + Backpin -f
        const QString wizPldPath = QDir(tempDir.path()).filePath(QStringLiteral("WizGates.pld"));
        const QString wizSiPath = QDir(tempDir.path()).filePath(QStringLiteral("WizGates.si"));
        {
            QFile wp(wizPldPath);
            if (wp.open(QIODevice::WriteOnly | QIODevice::Text)) wp.write(generatedPld.toLatin1());
            QFile ws(wizSiPath);
            if (ws.open(QIODevice::WriteOnly | QIODevice::Text)) ws.write(generatedSi.toLatin1());
        }
        win.openFile(wizPldPath, true);
        win.startCompile(true); // Device Independent ("virtual")
        const bool indepDone = win.waitForActiveProcess(15000);
        allOk &= check(indepDone && win.lastRunSucceeded(),
                       "Device-independent (virtual) compile on wizard-generated .PLD",
                       win.buildLogText().right(250));

        if (!ToolPaths::findExecutable(QStringLiteral("csim")).isEmpty()) {
            win.openFile(wizSiPath, true);
            win.startSimulate(true); // Device Independent ("virtual")
            const bool indepSimDone = win.waitForActiveProcess(15000);
            allOk &= check(indepSimDone && win.lastRunSucceeded(),
                           "Device-independent (virtual) simulate on wizard-generated .SI",
                           win.buildLogText().right(250));
        }

        if (!ToolPaths::findExecutable(QStringLiteral("backpin")).isEmpty() && !atmelDl.isEmpty()) {
            BackpinRequest breq;
            breq.sourcePldPath = wizPldPath;
            breq.deviceMnemonic = QStringLiteral("g16v8a");
            breq.libraryPath = atmelDl;
            breq.floatAllPins = true;
            win.openFile(wizPldPath, true);
            win.startBackpin(breq);
            const bool bpDone = win.waitForActiveProcess(15000);
            CodeEditor *reloadedEd = win.editorForPath(wizPldPath);
            allOk &= check(bpDone && win.lastProcessExitCode() == 0 &&
                               QFileInfo::exists(QDir(tempDir.path()).filePath(QStringLiteral("WizGates.bak"))) &&
                               reloadedEd != nullptr,
                           "Backpin -f floats pins, creates .bak backup, and reloads .PLD in editor",
                           win.buildLogText().right(250));

            // Should backpin write only part of the design, Float All Pins
            // and Nodes puts the design back; changed numbers alone are fine.
            allOk &= check(MainWindow::backpinKeptDesign(QStringLiteral("Pin 4 = a;\nPin [5,6] = [b0..1];\ny = a;\n"),
                                                         QStringLiteral("Pin   = a;\nPin   = [b0..1];\ny = a;\n")) &&
                               MainWindow::backpinKeptDesign(QStringLiteral("Pinnode = r;\r\n"),
                                                             QStringLiteral("Pinnode 615 = r;\r\n")) &&
                               !MainWindow::backpinKeptDesign(QStringLiteral("Pin 4 = a;\nNode n;\ny = a;\n"),
                                                              QStringLiteral("Pin   = a;\n")),
                           "Changes of pin and node numbers only are told apart from a cut-off design");
            const QString nodeDir = QDir(tempDir.path()).filePath(QStringLiteral("nodepins"));
            QDir().mkpath(nodeDir);
            const QString nodePld = QDir(nodeDir).filePath(QStringLiteral("NODEPINS.PLD"));
            const QByteArray nodeDesign =
                "Name NodePins; Partno x; Date x; Revision 01; Designer x; Company x; Assembly x; Location x;\r\n"
                "Device f1502plcc44;\r\n"
                "Pin 4 = a;\r\nPin 5 = b;\r\nPin 21 = y;\r\nNode n;\r\n"
                "n = a # b;\r\ny = n & a;\r\n/* end of design */\r\n";
            {
                QFile np(nodePld);
                if (np.open(QIODevice::WriteOnly)) np.write(nodeDesign);
            }
            win.openFile(nodePld, true);
            CodeEditor *nodeEd = win.editorForPath(nodePld);
            const QString nodeText = nodeEd ? nodeEd->toPlainText() : QString();
            win.startPinPlacement(MainWindow::PinPlacement::FloatAll);
            win.waitForActiveProcess(15000);
            QByteArray nodeAfter;
            {
                QFile np(nodePld);
                if (np.open(QIODevice::ReadOnly)) nodeAfter = np.readAll();
            }
            const QByteArray nodeFloated =
                "Name NodePins; Partno x; Date x; Revision 01; Designer x; Company x; Assembly x; Location x;\r\n"
                "Device f1502plcc44;\r\n"
                "Pin   = a;\r\nPin   = b;\r\nPin   = y;\r\nNode n;\r\n"
                "n = a # b;\r\ny = n & a;\r\n/* end of design */\r\n";
            allOk &= check(nodeAfter == nodeFloated && nodeEd &&
                               nodeEd->toPlainText() == QString::fromLatin1(nodeFloated).remove(QLatin1Char('\r')) &&
                               win.lastRunSucceeded() &&
                               !win.findChild<QMessageBox *>(QStringLiteral("backpinRestoredMessage")),
                           "Float All Pins and Nodes floats the pins of a design with a NODE statement",
                           win.buildLogText().right(500) + QString::fromLatin1(nodeAfter));

            // a statement backpin cannot rewrite leaves the design unchanged
            const QByteArray badDesign =
                "Name NodePins; Partno x; Date x; Revision 01; Designer x; Company x; Assembly x; Location x;\r\n"
                "Device f1502plcc44;\r\n"
                "Pin 4 = a\r\nPin 21 = y;\r\ny = a;\r\n";
            {
                QFile np(nodePld);
                if (np.open(QIODevice::WriteOnly)) np.write(badDesign);
            }
            win.startPinPlacement(MainWindow::PinPlacement::FloatAll);
            win.waitForActiveProcess(15000);
            QByteArray badAfter;
            {
                QFile np(nodePld);
                if (np.open(QIODevice::ReadOnly)) badAfter = np.readAll();
            }
            allOk &= check(badAfter == badDesign && !win.lastRunSucceeded() &&
                               win.buildLogText().contains(QStringLiteral("missing semi-colon on line above")) &&
                               !win.findChild<QMessageBox *>(QStringLiteral("backpinRestoredMessage")),
                           "Float All Pins and Nodes leaves a design unchanged that backpin cannot rewrite",
                           win.buildLogText().right(500) + QString::fromLatin1(badAfter));
        }
        // 9. Function out-params: ADDER.PLD drives C1..C3/Carry only through adder_slice()
        const QString adderSrc = examplesDir.isEmpty() ? QString()
                                                       : QDir(examplesDir).filePath(QStringLiteral("ADDER.PLD"));
        if (!adderSrc.isEmpty() && QFileInfo::exists(adderSrc)) {
            const QString adderPld = QDir(tempDir.path()).filePath(QStringLiteral("ADDER.PLD"));
            const QString adderSi = QDir(tempDir.path()).filePath(QStringLiteral("ADDER.si"));
            QFile::copy(adderSrc, adderPld);
            QFile af(adderPld);
            QString adderText;
            if (af.open(QIODevice::ReadOnly)) adderText = QString::fromLatin1(af.readAll());
            const PldDesignSummary adderSum = NewSimDialog::parsePldForSimulation(adderText);
            QStringList outs, ins;
            for (const PldSignalInfo &s : adderSum.signalsList) {
                (s.isOutput ? outs : ins) << s.name;
            }
            const bool rolesOk = outs.contains(QStringLiteral("C1")) && outs.contains(QStringLiteral("C3")) &&
                                 outs.contains(QStringLiteral("Carry")) && outs.contains(QStringLiteral("Z4")) &&
                                 ins.contains(QStringLiteral("X1")) && ins.contains(QStringLiteral("Y4")) &&
                                 ins.size() == 8 && outs.size() == 8;
            allOk &= check(rolesOk, "New .SI wizard maps function out-params (ADDER C1..C3, Carry) to outputs",
                           QStringLiteral("in=[%1] out=[%2]").arg(ins.join(QLatin1Char(',')), outs.join(QLatin1Char(','))));

            QFile sf(adderSi);
            if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                sf.write(NewSimDialog::generateSiTemplate(adderSum, 16).toLatin1());
                sf.close();
            }
            win.openFile(adderPld, true);
            win.startCompile(false);
            win.waitForActiveProcess(15000);
            const bool adderCompiled = win.lastRunSucceeded();
            win.openFile(adderSi, true);
            win.startSimulate(false);
            win.waitForActiveProcess(15000);
            allOk &= check(adderCompiled && win.lastRunSucceeded() && win.currentDiagnostics().isEmpty(),
                           "Wizard-generated ADDER.si simulates with zero csim errors",
                           QStringLiteral("diags=%1 %2").arg(win.currentDiagnostics().size())
                               .arg(win.buildLogText().right(250)));
        }

        // 10. Binary Table Editor + Define/Reference macro output compiles through cupl
        {
            const QString tbl = TableEditorDialog::generateTable(
                NewSimDialog::expandSignalListExpr(QStringLiteral("a, b")),
                NewSimDialog::expandSignalListExpr(QStringLiteral("y1..0")),
                {QStringLiteral("01"), QStringLiteral("10"), QStringLiteral("10"), QStringLiteral("01")});
            const QString macroDef = MacroDialog::formatDefinition(
                QStringLiteral("AND2"), {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("Y")},
                QStringLiteral("Y = A & B;"));
            const QList<CuplMacroDef> scanned = MacroDialog::scanMacros(macroDef);
            const QString macroRef = scanned.isEmpty()
                                         ? QString()
                                         : MacroDialog::formatReference(
                                               scanned.first(), {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("z")});
            allOk &= check(scanned.size() == 1 && scanned.first().params.size() == 3 &&
                               macroRef == QStringLiteral("AND2( a, b, z );\n") &&
                               tbl.contains(QStringLiteral("'b'11 => 'b'01;")),
                           "Table editor and macro Define/Reference generate CUPL syntax",
                           macroRef + tbl);

            const QList<CuplMacroDef> shipped = MacroDialog::discoverMacros(QString(), tempDir.path());
            bool hasT7442 = false;
            bool hasCounter = false;
            for (const CuplMacroDef &d : shipped) {
                if (d.name == QStringLiteral("T7442") && d.params.size() == 14) hasT7442 = true;
                if (d.name == QStringLiteral("Ten_Bit_Counter") && d.params.size() == 10) hasCounter = true;
            }
            allOk &= check(hasT7442 && hasCounter,
                           "Discover shipped Macro/*.MAC libraries (T7442/14, Ten_Bit_Counter/10)",
                           QString::number(shipped.size()));

            const QString tmPld = QDir(tempDir.path()).filePath(QStringLiteral("TBLMAC.PLD"));
            QFile tf(tmPld);
            if (tf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                tf.write(QStringLiteral(
                             "Name TblMac;\nPartno 01;\nDate 01/01/26;\nRevision 01;\nDesigner T;\n"
                             "Company T;\nAssembly None;\nLocation None;\nDevice g16v8a;\n"
                             "Pin 2 = a;\nPin 3 = b;\nPin 12 = y1;\nPin 13 = y0;\nPin 14 = z;\n")
                             .toLatin1());
                tf.write(macroDef.toLatin1());
                tf.write(tbl.toLatin1());
                tf.write(macroRef.toLatin1());
                tf.close();
            }
            win.openFile(tmPld, true);
            win.startCompile(false);
            win.waitForActiveProcess(15000);
            allOk &= check(win.lastRunSucceeded(), "Table + macro generated design compiles with cupl",
                           win.buildLogText().right(300));
        }

        // 11. Projects: the project's design is compiled whichever tab is
        // active; compiling another .PLD offers to make it the project.
        {
            const QString tblMacPld = QDir(tempDir.path()).filePath(QStringLiteral("TBLMAC.PLD"));
            allOk &= check(win.openProject(tempPldPath) &&
                               QFileInfo(win.projectPath()) == QFileInfo(tempPldPath) &&
                               win.windowTitle().contains(QStringLiteral("Project COUNT10")) &&
                               MainWindow::recentProjects().value(0) == win.projectPath() &&
                               QSettings().value(QStringLiteral("project/last")).toString() == win.projectPath(),
                           "Open project: title, recent projects and last project",
                           win.windowTitle());

            win.openFile(tblMacPld, true);
            QTreeWidget *sidebar = nullptr;
            for (QTreeWidget *tw : win.findChildren<QTreeWidget *>()) {
                if (tw->columnCount() == 3) sidebar = tw;
            }
            allOk &= check(sidebar && sidebar->topLevelItemCount() > 0 &&
                               sidebar->topLevelItem(0)->text(0).compare(QStringLiteral("COUNT10.PLD"),
                                                                         Qt::CaseInsensitive) == 0,
                           "Sidebar lists the project's files while another design is active");

            QStringList asked;
            win.setProjectSwitchPrompt([&asked](const QString &pld) {
                asked << pld;
                return MainWindow::ProjectSwitch::CompileProject;
            });
            win.startCompile(false);
            win.waitForActiveProcess(15000);
            const QString log1 = win.buildLogText();
            allOk &= check(asked == QStringList{QStringLiteral("TBLMAC.PLD")} && win.lastRunSucceeded() &&
                               log1.contains(QStringLiteral("COUNT10.PLD")) && !log1.contains(QStringLiteral("TBLMAC")),
                           "Compile with another .PLD active asks, then compiles the project",
                           asked.join(QLatin1Char(',')) + QLatin1Char(' ') + log1.left(200));

            win.setProjectSwitchPrompt([](const QString &) { return MainWindow::ProjectSwitch::Cancel; });
            win.startCompile(false);
            allOk &= check(win.buildLogText() == log1 && QFileInfo(win.projectPath()) == QFileInfo(tempPldPath),
                           "Cancelling the question compiles nothing");

            win.setProjectSwitchPrompt([](const QString &) {
                return MainWindow::ProjectSwitch::CompileActiveAsProject;
            });
            win.startCompile(false);
            win.waitForActiveProcess(15000);
            allOk &= check(QFileInfo(win.projectPath()) == QFileInfo(tblMacPld) && win.lastRunSucceeded() &&
                               win.buildLogText().contains(QStringLiteral("TBLMAC.PLD")) &&
                               win.windowTitle().contains(QStringLiteral("Project TBLMAC")),
                           "Compiling another .PLD can make it the project",
                           win.windowTitle());
            win.setProjectSwitchPrompt(nullptr);

            {
                MainWindow restored;
                restored.restoreLastProject();
                allOk &= check(QFileInfo(restored.projectPath()) == QFileInfo(tblMacPld),
                               "Last project is reopened at startup", restored.projectPath());
            }

            allOk &= check(win.closeProject() && win.projectPath().isEmpty() &&
                               win.editorForPath(tblMacPld) == nullptr &&
                               !win.windowTitle().contains(QStringLiteral("Project")) &&
                               !QSettings().contains(QStringLiteral("project/last")),
                           "Close project closes its tabs and leaves project mode", win.windowTitle());
        }

        // 12. User tools: kept in QSettings, shown in the Tools menu and on
        // the toolbar, run with placeholders filled in from the project.
        {
            ToolContext ctx;
            ctx.design = QStringLiteral("/work/my designs/CNT.pld");
            ctx.dir = QStringLiteral("/work/my designs");
            ctx.name = QStringLiteral("CNT");
            ctx.device = QStringLiteral("f1504plcc44");
            ctx.jed = QStringLiteral("/work/my designs/CNT.jed");
            const QStringList expanded = UserTools::expandArguments(
                QStringLiteral("-p $(device) -w \"$(JED)\" --name=$(NAME).bin $(UNKNOWN)"), ctx);
            const QStringList expected = {
                QStringLiteral("-p"), QStringLiteral("f1504plcc44"), QStringLiteral("-w"),
                QStringLiteral("/work/my designs/CNT.jed"), QStringLiteral("--name=CNT.bin"),
                QStringLiteral("$(UNKNOWN)")};
            allOk &= check(expanded == expected, "User tool placeholders expand within quoted arguments",
                           expanded.join(QLatin1Char('|')));

            const QString echoExe = QStringLiteral("/bin/echo");
            const QString sleepExe = QStringLiteral("/bin/sleep");
            UserTool echoTool;
            echoTool.menuName = QStringLiteral("Echo Design");
            echoTool.toolTip = QStringLiteral("Echo the design's files");
            echoTool.command = echoExe;
            echoTool.arguments = QStringLiteral("name=$(NAME) device=$(DEVICE) jed=$(JED)");
            echoTool.onToolbar = true;
            UserTool sleepTool;
            sleepTool.menuName = QStringLiteral("Sleep");
            sleepTool.command = sleepExe;
            sleepTool.arguments = QStringLiteral("30");
            sleepTool.saveFirst = false;
            win.setUserTools({echoTool, sleepTool});

            QToolBar *toolBar = win.findChild<QToolBar *>(QStringLiteral("UserToolsToolBar"));
            const QList<UserTool> reloaded = UserTool::loadFromSettings();
            allOk &= check(reloaded.size() == 2 && reloaded.at(0) == echoTool && reloaded.at(1) == sleepTool &&
                               toolBar && !toolBar->actions().isEmpty() &&
                               toolBar->actions().first()->toolTip() == echoTool.toolTip,
                           "User tools persist and appear on the toolbar");

            if (QFileInfo(echoExe).isExecutable() && QFileInfo(sleepExe).isExecutable()) {
                win.openProject(tempPldPath);
                win.runUserTool(0);
                win.waitForActiveProcess(10000);
                const QString log = win.buildLogText();
                allOk &= check(win.lastRunSucceeded() && log.contains(QStringLiteral("name=COUNT10 device=g16v8a jed=")) &&
                                   log.contains(QStringLiteral("COUNT10.jed"), Qt::CaseInsensitive),
                               "User tool runs with $(NAME), $(DEVICE) and $(JED) of the project",
                               log.right(300));

                win.runUserTool(1);
                QElapsedTimer t;
                t.start();
                QTimer::singleShot(300, &win, [&win]() { win.stopRunningTool(); });
                const bool stopped = win.waitForActiveProcess(10000);
                allOk &= check(stopped && t.elapsed() < 5000 && !win.lastRunSucceeded() &&
                                   win.buildLogText().contains(QStringLiteral("Stopped by the user")),
                               "A running user tool can be stopped",
                               QStringLiteral("%1 ms").arg(t.elapsed()));
                win.closeProject();
            } else {
                std::fprintf(stdout, "[INFO] /bin/echo or /bin/sleep missing; skipped running user tools.\n");
            }
            win.setUserTools({});
            allOk &= check(UserTool::loadFromSettings().isEmpty() && toolBar && toolBar->isHidden(),
                           "Removing all user tools empties and hides the toolbar");
        }

        // 13. Float All Pins and Nodes, then compile (the fitter places the
        // pins), then Assign Pins and Pinnodes, on an ATF1504 example.
        const QString jkSrc = examplesDir.isEmpty() ? QString()
                                                    : QDir(examplesDir).filePath(QStringLiteral("JKTEST.PLD"));
        if (!jkSrc.isEmpty() && QFileInfo::exists(jkSrc) &&
            !ToolPaths::findExecutable(QStringLiteral("backpin")).isEmpty() &&
            !ToolPaths::findExecutable(QStringLiteral("fit1504")).isEmpty()) {
            const QString jkDir = QDir(tempDir.path()).filePath(QStringLiteral("pins"));
            QDir().mkpath(jkDir);
            const QString jkPld = QDir(jkDir).filePath(QStringLiteral("JKTEST.PLD"));
            QFile::copy(jkSrc, jkPld);
            QFile::setPermissions(jkPld, QFile::ReadOwner | QFile::WriteOwner);
            static const QRegularExpression numberedPin(QStringLiteral("^\\s*PIN\\s+\\d+\\s*=\\s*\\w+"),
                                                        QRegularExpression::CaseInsensitiveOption |
                                                            QRegularExpression::MultilineOption);
            auto numberedPins = [](const QString &text) {
                int n = 0;
                for (auto it = numberedPin.globalMatch(text); it.hasNext(); it.next()) ++n;
                return n;
            };

            win.openProject(jkPld);
            CodeEditor *jkEd = win.editorForPath(jkPld);
            const QString original = jkEd ? jkEd->toPlainText() : QString();
            const MainWindow::PinPlacementPlan early = win.planPinPlacement(MainWindow::PinPlacement::AssignPinsAndNodes);
            allOk &= check(early.error.contains(QStringLiteral("Device Dependent Compile")),
                           "Assign Pins before a compile explains that the fitter's placements are missing",
                           early.error);

            win.startPinPlacement(MainWindow::PinPlacement::FloatAll);
            win.waitForActiveProcess(15000);
            const QString floated = jkEd ? jkEd->toPlainText() : QString();
            allOk &= check(win.lastProcessExitCode() == 0 && numberedPins(original) == 4 &&
                               numberedPins(floated) == 0 && floated.contains(QStringLiteral("CLK")) &&
                               QFileInfo::exists(QDir(jkDir).filePath(QStringLiteral("JKTEST.bak"))) &&
                               !jkEd->document()->isModified(),
                           "Float All Pins and Nodes removes the pin numbers and reloads the design",
                           win.buildLogText().right(300));
            jkEd->undo();
            const bool undoOk = jkEd->toPlainText() == original;
            jkEd->redo();
            allOk &= check(undoOk && jkEd->toPlainText() == floated && !jkEd->document()->isModified(),
                           "Undo brings back the design as it was before backpin");

            win.startCompile(false);
            win.waitForActiveProcess(30000);
            allOk &= check(win.lastRunSucceeded() && QFileInfo::exists(QDir(jkDir).filePath(QStringLiteral("JKTEST.pin"))),
                           "Floated ATF1504 design compiles and the fitter places its pins",
                           win.buildLogText().right(300));

            win.startPinPlacement(MainWindow::PinPlacement::AssignPinsAndNodes);
            win.waitForActiveProcess(15000);
            const QString assigned = jkEd->toPlainText();
            allOk &= check(win.lastProcessExitCode() == 0 && numberedPins(assigned) == 4 &&
                               !QFileInfo::exists(QDir(jkDir).filePath(QStringLiteral("JKTEST.plc"))) &&
                               win.buildLogText().contains(QStringLiteral("JKTEST.pin")),
                           "Assign Pins and Pinnodes writes the fitter's pin numbers back",
                           win.buildLogText().right(400));

            win.startCompile(false);
            win.waitForActiveProcess(30000);
            win.startPinPlacement(MainWindow::PinPlacement::AssignPinsOnly);
            win.waitForActiveProcess(15000);
            allOk &= check(win.lastProcessExitCode() == 0 && jkEd->toPlainText() == assigned,
                           "Assign Pins Only keeps the assigned design unchanged",
                           win.buildLogText().right(300));
            win.closeProject();
        } else {
            std::fprintf(stdout, "[INFO] JKTEST.PLD, backpin or fit1504 missing; skipped pin placement test.\n");
        }
    } else if (allowMissingTools) {
        std::fprintf(stdout, "[INFO] Sibling 'cupl' binary not found; skipped live compile step (allowed).\n");
    } else {
        allOk &= check(false, "Live compile/simulate steps require sibling 'cupl'");
    }

    allOk &= checkHelp();

    if (allOk) {
        std::fprintf(stdout, "=== lincupl --self-test PASSED ===\n");
        return 0;
    }
    std::fprintf(stderr, "=== lincupl --self-test FAILED ===\n");
    return 1;
}

} // namespace SelfTest
