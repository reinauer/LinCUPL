#include "MainWindow.h"
#include "SelfTest.h"
#include "../common/app_icon.h"
#include "../common/qt_macos_accessibility.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QStatusBar>
#include <QTimer>

#include <cstdio>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    installMacAccessibilityWorkaround();
    QApplication::setApplicationName(QStringLiteral("linsim"));
    QApplication::setApplicationDisplayName(QStringLiteral("LinSim"));
    QApplication::setOrganizationName(QStringLiteral("LinCUPL"));
    QApplication::setApplicationVersion(QStringLiteral(LINCUPL_VERSION));
    lincuplSetAppIcon(QStringLiteral("linsim"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("LinSim — Interactive CUPL Digital Waveform Simulator & Test-Vector Editor (.SI / .WO)"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption selfTestOpt(
        QStringLiteral("self-test"),
        QStringLiteral("Run headless parser, CSIM simulation, round-trip, and offscreen rendering self-tests."));
    parser.addOption(selfTestOpt);

    QCommandLineOption quitAfterOpt(
        QStringLiteral("quit-after"),
        QStringLiteral("Automatically close window after <ms> milliseconds (for automated smoke tests)."),
        QStringLiteral("ms"));
    parser.addOption(quitAfterOpt);

    parser.addPositionalArgument(
        QStringLiteral("file"),
        QStringLiteral("Optional simulation input (.si) or waveform output (.wo) file to open."),
        QStringLiteral("[file.si|file.wo]"));

    parser.process(app);

    if (parser.isSet(selfTestOpt)) {
        return runLinSimSelfTest();
    }

    // Each file opens in a window of its own; the program ends when the last
    // window is closed.
    MainWindow *win = MainWindow::newWindow();
    for (const QString &path : parser.positionalArguments()) {
        QString err;
        if (!MainWindow::openInWindow(path, win, &err)) {
            std::fprintf(stderr, "linsim: warning: %s\n", err.toUtf8().constData());
            win->statusBar()->showMessage(err, 8000);
        }
    }

    if (parser.isSet(quitAfterOpt)) {
        bool ok = false;
        const int ms = parser.value(quitAfterOpt).toInt(&ok);
        if (ok && ms >= 0) {
            QTimer::singleShot(ms, &app, &QCoreApplication::quit);
        }
    }

    return app.exec();
}
