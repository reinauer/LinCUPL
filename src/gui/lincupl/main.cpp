#include "mainwindow.h"
#include "selftest.h"
#include "../common/app_icon.h"
#include "../common/qt_macos_accessibility.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTimer>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    installMacAccessibilityWorkaround();
    QApplication::setOrganizationName(QStringLiteral("LinCUPL"));
    QApplication::setApplicationName(QStringLiteral("LinCUPL"));
    QApplication::setApplicationVersion(QStringLiteral(LINCUPL_VERSION));
    lincuplSetAppIcon(QStringLiteral("lincupl"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("LinCUPL - Native Qt6 IDE for CUPL Programmable Logic Compilation & Simulation"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption selfTestOpt(
        QStringLiteral("self-test"),
        QStringLiteral("Run automated headless self-test suite and exit."));
    parser.addOption(selfTestOpt);

    QCommandLineOption quitAfterOpt(
        QStringLiteral("quit-after"),
        QStringLiteral("Automatically exit event loop after <ms> milliseconds (for headless testing)."),
        QStringLiteral("ms"));
    parser.addOption(quitAfterOpt);

    parser.addPositionalArgument(
        QStringLiteral("files"),
        QStringLiteral("Optional .PLD, .SI, or output files to open at startup."),
        QStringLiteral("[files...]"));

    parser.process(app);

    if (parser.isSet(selfTestOpt)) {
        return SelfTest::run();
    }

    MainWindow window;
    window.restoreLastProject();
    const QStringList positional = parser.positionalArguments();
    for (const QString &path : positional) {
        if (QFileInfo::exists(path)) {
            window.openFile(path, true);
        }
    }
    window.show();

    if (parser.isSet(quitAfterOpt)) {
        const int ms = qMax(10, parser.value(quitAfterOpt).toInt());
        QTimer::singleShot(ms, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
