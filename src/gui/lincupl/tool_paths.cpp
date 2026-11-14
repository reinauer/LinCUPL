#include "tool_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace ToolPaths {

namespace {

QStringList candidateRootDirs() {
    QStringList roots;
    auto addAncestors = [&roots](const QString &startPath) {
        if (startPath.isEmpty()) {
            return;
        }
        QDir dir(startPath);
        for (int depth = 0; depth < 6 && dir.exists(); ++depth) {
            const QString clean = QDir::cleanPath(dir.absolutePath());
            if (!roots.contains(clean)) {
                roots.append(clean);
            }
            if (!dir.cdUp()) {
                break;
            }
        }
    };

    if (QCoreApplication::instance()) {
        addAncestors(QCoreApplication::applicationDirPath());
    }
    addAncestors(QDir::currentPath());

    const QString envShare = qEnvironmentVariable("LINCUPL_SHARE");
    if (!envShare.isEmpty()) {
        addAncestors(envShare);
    }

#ifdef LINCUPL_SOURCE_ROOT
    addAncestors(QStringLiteral(LINCUPL_SOURCE_ROOT));
#endif
    return roots;
}

bool isExecutableFile(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }
    QFileInfo fi(path);
    return fi.exists() && fi.isFile() && fi.isExecutable();
}

QString findDataFile(const QStringList &relativeNames) {
    const QString envShare = qEnvironmentVariable("LINCUPL_SHARE");
    if (!envShare.isEmpty()) {
        for (const QString &rel : relativeNames) {
            const QString fname = QFileInfo(rel).fileName();
            QFileInfo direct(QDir(envShare).filePath(fname));
            if (direct.exists() && direct.isFile()) {
                return direct.canonicalFilePath();
            }
        }
    }

    const QStringList roots = candidateRootDirs();
    for (const QString &root : roots) {
        QDir d(root);
        for (const QString &rel : relativeNames) {
            QFileInfo fi(d.filePath(rel));
            if (fi.exists() && fi.isFile()) {
                return fi.canonicalFilePath();
            }
        }
    }
    return QString();
}

} // namespace

QString customBinDir() {
    QSettings settings;
    return settings.value(QStringLiteral("paths/customBinDir")).toString().trimmed();
}

void setCustomBinDir(const QString &dirPath) {
    QSettings settings;
    settings.setValue(QStringLiteral("paths/customBinDir"), dirPath.trimmed());
}

QString findExecutable(const QString &toolName) {
#ifdef Q_OS_WIN
    const QString exeName = toolName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
                                ? toolName
                                : (toolName + QStringLiteral(".exe"));
#else
    const QString exeName = toolName;
#endif

    const QString customDir = customBinDir();
    if (!customDir.isEmpty()) {
        const QString customCandidate = QDir(customDir).filePath(exeName);
        if (isExecutableFile(customCandidate)) {
            return QFileInfo(customCandidate).canonicalFilePath();
        }
    }

    if (QCoreApplication::instance()) {
        const QDir appDir(QCoreApplication::applicationDirPath());
        const QStringList localRel = {
            exeName,
            QStringLiteral("../bin/") + exeName,
            QStringLiteral("../../bin/") + exeName,
        };
        for (const QString &rel : localRel) {
            const QString p = appDir.filePath(rel);
            if (isExecutableFile(p)) {
                return QFileInfo(p).canonicalFilePath();
            }
        }
    }

    const QStringList subDirs = {
        QStringLiteral("bin"),
        QStringLiteral("build/bin"),
        QStringLiteral("build"),
        QStringLiteral("src/gui/linsim"),
        QStringLiteral("src/gui/lincupl"),
    };
    for (const QString &root : candidateRootDirs()) {
        const QDir d(root);
        if (isExecutableFile(d.filePath(exeName))) {
            return QFileInfo(d.filePath(exeName)).canonicalFilePath();
        }
        for (const QString &sub : subDirs) {
            const QString candidate = d.filePath(sub + QLatin1Char('/') + exeName);
            if (isExecutableFile(candidate)) {
                return QFileInfo(candidate).canonicalFilePath();
            }
        }
    }

    const QString fromPath = QStandardPaths::findExecutable(exeName);
    if (isExecutableFile(fromPath)) {
        return fromPath;
    }
    return QString();
}

QStringList findDeviceLibraries() {
    QStringList result;
    auto addIfValid = [&result](const QString &path) {
        if (path.isEmpty()) {
            return;
        }
        QFileInfo fi(path);
        if (fi.exists() && fi.isFile()) {
            const QString canon = fi.canonicalFilePath();
            if (!canon.isEmpty() && !result.contains(canon)) {
                result.append(canon);
            }
        }
    };

    QSettings settings;
    addIfValid(settings.value(QStringLiteral("paths/deviceLibrary")).toString());

    const QString envLib = qEnvironmentVariable("LIBCUPL");
    addIfValid(envLib);

    const QString envShare = qEnvironmentVariable("LINCUPL_SHARE");
    if (!envShare.isEmpty()) {
        const QDir sd(envShare);
        addIfValid(sd.filePath(QStringLiteral("lincupl.dl")));
        addIfValid(sd.filePath(QStringLiteral("Atmel.dl")));
        addIfValid(sd.filePath(QStringLiteral("atmel.dl")));
        addIfValid(sd.filePath(QStringLiteral("cupl.dl")));
        addIfValid(sd.filePath(QStringLiteral("CUPL.DL")));
    }

    const QStringList relCandidates = {
        QStringLiteral("lincupl.dl"),
        QStringLiteral("Atmel.dl"),
        QStringLiteral("cupl.dl"),
        QStringLiteral("../share/lincupl/lincupl.dl"),
        QStringLiteral("../share/lincupl/Atmel.dl"),
        QStringLiteral("../share/lincupl/cupl.dl"),
        QStringLiteral("share/lincupl/lincupl.dl"),
        QStringLiteral("share/lincupl/Atmel.dl"),
        QStringLiteral("share/lincupl/cupl.dl"),
    };

    for (const QString &root : candidateRootDirs()) {
        const QDir d(root);
        for (const QString &rel : relCandidates) {
            addIfValid(d.filePath(rel));
        }
    }

    return result;
}

QString activeDeviceLibrary() {
    QSettings settings;
    const QString saved = settings.value(QStringLiteral("paths/deviceLibrary")).toString().trimmed();
    if (!saved.isEmpty() && QFileInfo::exists(saved)) {
        return QFileInfo(saved).canonicalFilePath();
    }

    const QString envLib = qEnvironmentVariable("LIBCUPL");
    if (!envLib.isEmpty() && QFileInfo::exists(envLib)) {
        return QFileInfo(envLib).canonicalFilePath();
    }

    const QStringList libs = findDeviceLibraries();
    // Prefer lincupl.dl (generated at build time: Atmel.dl's devices, then
    // those only cupl.dl has), then Atmel.dl, else the first one found
    for (const QString &name : {QStringLiteral("lincupl.dl"), QStringLiteral("Atmel.dl")}) {
        for (const QString &lib : libs) {
            if (QFileInfo(lib).fileName().compare(name, Qt::CaseInsensitive) == 0) {
                return lib;
            }
        }
    }
    return libs.isEmpty() ? QString() : libs.first();
}

void setActiveDeviceLibrary(const QString &path) {
    QSettings settings;
    settings.setValue(QStringLiteral("paths/deviceLibrary"), path.trimmed());
}

QString findAtmelCat() {
    return findDataFile({
        QStringLiteral("Atmel.cat"),
        QStringLiteral("../share/lincupl/Atmel.cat"),
        QStringLiteral("share/lincupl/Atmel.cat"),
    });
}

QString findCuplErrTxt() {
    return findDataFile({
        QStringLiteral("CuplErr.txt"),
        QStringLiteral("../share/lincupl/CuplErr.txt"),
        QStringLiteral("share/lincupl/CuplErr.txt"),
    });
}

QString findExamplesDir() {
    const QString envShare = qEnvironmentVariable("LINCUPL_SHARE");
    if (!envShare.isEmpty()) {
        QFileInfo fi(QDir(envShare).filePath(QStringLiteral("Examples/Atmel")));
        if (fi.isDir()) {
            return fi.canonicalFilePath();
        }
    }
    for (const QString &root : candidateRootDirs()) {
        const QDir d(root);
        const QStringList rels = {
            QStringLiteral("../share/lincupl/Examples/Atmel"),
            QStringLiteral("share/lincupl/Examples/Atmel"),
            QStringLiteral("Examples/Atmel"),
        };
        for (const QString &rel : rels) {
            QFileInfo fi(d.filePath(rel));
            if (fi.exists() && fi.isDir()) {
                return fi.canonicalFilePath();
            }
        }
    }
    return QString();
}

QString findMacroDir() {
    const QString envShare = qEnvironmentVariable("LINCUPL_SHARE");
    if (!envShare.isEmpty()) {
        QFileInfo fi(QDir(envShare).filePath(QStringLiteral("Macro")));
        if (fi.isDir()) {
            return fi.canonicalFilePath();
        }
    }
    for (const QString &root : candidateRootDirs()) {
        const QDir d(root);
        const QStringList rels = {
            QStringLiteral("../share/lincupl/Macro"),
            QStringLiteral("share/lincupl/Macro"),
        };
        for (const QString &rel : rels) {
            QFileInfo fi(d.filePath(rel));
            if (fi.exists() && fi.isDir()) {
                return fi.canonicalFilePath();
            }
        }
    }
    return QString();
}

} // namespace ToolPaths
